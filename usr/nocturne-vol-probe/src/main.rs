// /bin/nocturne-vol-probe -- the N-3a-2 sink-volume witness (docs/NOCTURNE.md
// 6.8/6.10, I-46). Two arms, one variable apart, BOTH over DIRECT /srv/nocturne
// connections -- never joey's shared /dev/nocturne mount, whose server-side peer
// is the MOUNTER (SYSTEM), so a write through it could never exercise the
// per-caller gate (the Warp F1 / libtapestry per-connection idiom):
//
//   PARENT (this process, SYSTEM): a direct-conn volume write is ACCEPTED and
//   the Plan 9 volume(3) grammar round-trips -- audio/mix, one value or L R,
//   mute (audio 0), and an unknown control -> EINVAL. The positive gate arm
//   (the SYSTEM axis) plus the file itself.
//
//   DENY child (spawned by the parent with a USER identity; argv "deny"): the
//   SAME direct-conn volume write is REFUSED (EPERM) -- the child is not SYSTEM,
//   not console-attached, and holds no CAP_AUDIO_GRAPH. This is the negative
//   arm, and it is not optional: without it a `return true` gate would pass the
//   positive alone (M-PIN -- a control must prove discrimination). Setting the
//   child's principal needs CAP_SET_IDENTITY, which joey confers in the spawn
//   mask (the caps-probe pattern).
//
// The gain-stage attenuation (a volume write actually scaling the mix) is a
// float multiply before the I-14 clamp; the round-trip proves the fields are
// set, and a wav witness of the audio effect is deferred to the N-3d volume-OSD
// chunk that drives it end to end.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env;
use libthyla_rs::io::Write;
use libthyla_rs::process::{Command, Stdio};
use libthyla_rs::{
    t_close, t_open, t_putstr, t_read, t_write, T_OREAD, T_OWRITE, T_WALK_OPEN_FROM_ROOT,
};

const NOC_SRV: &[u8] = b"/srv/nocturne";
const SELF_BIN: &str = "/bin/nocturne-vol-probe";
// Any real, non-SYSTEM principal: the gate is on the caller's identity/caps, so
// the value only has to pass the id-validity check (caps-probe's OTHER_PRINCIPAL).
const DENY_PRINCIPAL: u32 = 1;

/// Write to BOTH the kernel console (serial log) and fd 1 -- joey's
/// pouch_smoke harness checks fd 1 for the PASS marker, and the console copy
/// makes the probe legible from a tile whose fd 1 is a pts.
fn say(s: &str) {
    let _ = t_putstr(s);
    let mut out = libthyla_rs::io::stdout();
    let _ = out.write_all(s.as_bytes());
}

fn fail(why: &str) -> i64 {
    let mut s = String::from("NOCTURNE-VOL-PROBE FAIL: ");
    s.push_str(why);
    s.push('\n');
    say(&s);
    1
}

/// Connect DIRECTLY to /srv/nocturne -- a fresh per-client server conn, so the
/// peer nocturned resolves via SYS_SRV_PEER is US, not the /dev/nocturne
/// mounter. Returns the session-root handle, or <0 on failure.
fn connect() -> i64 {
    unsafe { t_open(T_WALK_OPEN_FROM_ROOT, NOC_SRV.as_ptr(), NOC_SRV.len(), T_OREAD) }
}

/// Write `msg` to the volume file on `root`; returns t_write's result (>=0 count
/// on success, <0 errno on refusal). A fresh volume fd each call -- the file has
/// no write offset, and this keeps open (ungated) distinct from write (gated).
fn write_volume(root: i64, msg: &[u8]) -> i64 {
    let vol = unsafe { t_open(root, b"volume".as_ptr(), 6, T_OWRITE) };
    if vol < 0 {
        return vol;
    }
    let n = unsafe { t_write(vol, msg.as_ptr(), msg.len()) };
    unsafe { t_close(vol) };
    n
}

/// True iff the volume file's rendered text contains `needle`.
fn volume_has(root: i64, needle: &[u8]) -> bool {
    let vol = unsafe { t_open(root, b"volume".as_ptr(), 6, T_OREAD) };
    if vol < 0 {
        return false;
    }
    let mut buf = [0u8; 128];
    let n = unsafe { t_read(vol, buf.as_mut_ptr(), buf.len()) };
    unsafe { t_close(vol) };
    if n <= 0 {
        return false;
    }
    let got = &buf[..n as usize];
    !needle.is_empty() && got.windows(needle.len()).any(|w| w == needle)
}

/// The DENY child (a user principal): a direct-conn volume write MUST be refused.
/// Exit 0 iff the gate refused it (the correct outcome); non-zero if the write
/// was accepted (the gate is bypassed) or the control (connect) is broken.
fn deny_child() -> i64 {
    let root = connect();
    if root < 0 {
        say("NOCTURNE-VOL-DENY FAIL: child could not connect /srv/nocturne\n");
        return 3;
    }
    // The gate is at WRITE, not open (the file is mode 0o666): a refused write
    // over a live connection is the gate, not a broken path.
    let n = write_volume(root, b"audio 50\n");
    unsafe { t_close(root) };
    if n < 0 {
        say("NOCTURNE-VOL-DENY OK: user-principal direct write refused (EPERM)\n");
        0
    } else {
        say("NOCTURNE-VOL-DENY FAIL: user-principal write ACCEPTED -- gate bypassed\n");
        1
    }
}

fn parent() -> i64 {
    let root = connect();
    if root < 0 {
        return fail("connect /srv/nocturne");
    }
    // Default is unity on both controls.
    if !volume_has(root, b"audio 100 100") || !volume_has(root, b"mix 100 100") {
        return fail("default volume was not 'audio 100 100' + 'mix 100 100'");
    }
    // Positive gate (SYSTEM axis) + the grammar parse/render round-trip.
    if write_volume(root, b"audio 50\n") < 0 {
        return fail("SYSTEM 'audio 50' write was refused");
    }
    if !volume_has(root, b"audio 50 50") {
        return fail("read-back after 'audio 50' was not 'audio 50 50'");
    }
    if write_volume(root, b"audio 70 90\n") < 0 || !volume_has(root, b"audio 70 90") {
        return fail("'audio 70 90' (per-channel) round-trip");
    }
    if write_volume(root, b"mix 40\n") < 0 || !volume_has(root, b"mix 40 40") {
        return fail("'mix 40' round-trip");
    }
    // An unknown control is refused (EINVAL), leaving the gain unchanged.
    if write_volume(root, b"bogus 1\n") >= 0 {
        return fail("invalid control 'bogus 1' was ACCEPTED");
    }
    // Mute, then restore unity.
    if write_volume(root, b"audio 0\n") < 0 || !volume_has(root, b"audio 0 0") {
        return fail("'audio 0' (mute) round-trip");
    }
    if write_volume(root, b"audio 100\n") < 0 || write_volume(root, b"mix 100\n") < 0 {
        return fail("reset to unity");
    }
    unsafe { t_close(root) };

    // Negative gate: a user-principal direct write MUST be refused. The parent
    // holds CAP_SET_IDENTITY (joey's spawn mask), so it can stamp the child's
    // principal; the child inherits this namespace (so it sees /srv/nocturne)
    // but not SYSTEM, not the console, and no clearance.
    // stderr -> a fresh pipe, NOT Inherit: joey wires this probe only fd 0+1
    // (pouch_smoke's 2-fd pipe), so inheriting the missing fd 2 would fail the
    // spawn. The child writes via SYS_PUTS (the console) regardless, so it needs
    // no real stderr; Piped just hands it a valid fd 2 without an inherit.
    match Command::new(SELF_BIN)
        .arg("deny")
        .identity(DENY_PRINCIPAL, DENY_PRINCIPAL, &[])
        .caps(0)
        .stderr(Stdio::Piped)
        .spawn()
    {
        Ok(mut child) => match child.wait() {
            Ok(st) if st.success() => {}
            Ok(_) => return fail("deny child reported the gate was bypassed"),
            Err(_) => return fail("deny child wait() failed"),
        },
        Err(_) => return fail("could not spawn the deny child (missing CAP_SET_IDENTITY?)"),
    }

    say("NOCTURNE-VOL-PROBE PASS (grammar round-trip + SYSTEM allow + user-principal deny)\n");
    0
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let deny = env::args().operands().next().map_or(false, |a| a == b"deny");
    if deny {
        deny_child()
    } else {
        parent()
    }
}
