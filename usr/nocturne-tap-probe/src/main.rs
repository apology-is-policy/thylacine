// /bin/nocturne-tap-probe -- the N-3c-1 sink-tap (capture) authority witness
// (docs/NOCTURNE.md 6.4/6.8, I-46). The sink tap is an EAR on the mixed output:
// reading it is recording, so it is eavesdropping unless the reader holds the
// sink authority. The tap lives ONLY on the per-connection /srv/nocturne-ctl
// post (the peer is the reader) and NEVER on joey's shared /dev/nocturne mount
// (whose peer is always the mounter=SYSTEM -- the N-3a-2 F1 lesson). This witness
// proves the surface BOTH ways, and the arm that matters most is the one an
// eavesdropper would take: a mount `/dev/nocturne/audio` READ must be REFUSED,
// and a USER opening the -ctl `tap` must be DENIED.
//
//   PARENT (SYSTEM):
//     - opening /srv/nocturne-ctl/tap is ACCEPTED (the SYSTEM axis).
//     - the single-reader guard: a SECOND concurrent tap open is EBUSY.
//     - a played tone is CAPTURED: with the tap open, write a loud tone to the
//       playback voice and read back non-silence on the tap (the positive arm --
//       without it a gate that refused every read would pass the denials alone).
//     - the eavesdrop-via-mount regression: a /dev/nocturne/audio READ is REFUSED
//       (recording is the gated -ctl tap, never the shared mount).
//
//   DENY child (a USER principal; argv "deny") -- BOTH must be refused:
//     - opening the -ctl `tap` is DENIED (EPERM): not SYSTEM, no CAP_AUDIO_GRAPH,
//       not the console-owner session.
//     - a /dev/nocturne/audio READ is REFUSED (the mount carries no tap).
//
// The CAP_AUDIO_GRAPH-admit axis is covered by test_devcap.clearance_audio_graph
// and the console-owner axis by proc_identity.peer_snapshot_console_owner; a boot
// probe holds neither, so it witnesses the SYSTEM allow + the two user/mount
// denials here. The per-read fresh re-check (mid-recording revocation) is prose-
// argued + code-audited (round 7); a boot probe cannot mutate its own caps.

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

// The sink-authority post (per-connection; the gate reads THIS program as peer).
const NOC_CTL: &[u8] = b"/srv/nocturne-ctl";
// The playback mount's audio node: a WRITE plays (voice 0); a READ is the
// eavesdrop-via-mount path and must be refused (N-3c-1).
const MOUNT_AUDIO: &[u8] = b"/dev/nocturne/audio";
const SELF_BIN: &str = "/bin/nocturne-tap-probe";
const DENY_PRINCIPAL: u32 = 1;

/// Write to BOTH the kernel console and fd 1 (joey's pouch_smoke checks fd 1).
fn say(s: &str) {
    let _ = t_putstr(s);
    let mut out = libthyla_rs::io::stdout();
    let _ = out.write_all(s.as_bytes());
}

fn fail(why: &str) -> i64 {
    let mut s = String::from("NOCTURNE-TAP-PROBE FAIL: ");
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

/// Open the sink `tap` on a -ctl connection. Returns the fd (<0 on refusal).
fn open_tap(ctl: i64) -> i64 {
    unsafe { t_open(ctl, b"tap".as_ptr(), 3, T_OREAD) }
}

/// Play a loud constant tone into the playback voice via the mount audio file.
/// True iff every write was accepted. (The write path is unchanged by N-3c-1;
/// only the READ of this file is refused.)
fn play_tone() -> bool {
    let a = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, MOUNT_AUDIO.as_ptr(), MOUNT_AUDIO.len(), T_OWRITE) };
    if a < 0 {
        return false;
    }
    // A loud constant S16 sample (0x2000 per channel) -- non-silent by a wide
    // margin so any captured period trips the non-zero assertion.
    let mut buf = [0u8; 4096];
    let s = 0x2000i16.to_le_bytes();
    for f in buf.chunks_mut(2) {
        f.copy_from_slice(&s);
    }
    let mut ok = true;
    for _ in 0..8 {
        if unsafe { t_write(a, buf.as_ptr(), buf.len()) } < 0 {
            ok = false;
            break;
        }
    }
    unsafe { t_close(a) };
    ok
}

/// Read the tap (bounded) until a non-silent chunk is seen. True iff any non-zero
/// byte was captured -- i.e. the tap carries the sink mix. The tap read parks
/// server-side until the mixer produces a period, so a few reads suffice.
fn tap_captures_nonsilence(tap: i64) -> bool {
    let mut buf = [0u8; 4096];
    for _ in 0..16 {
        let n = unsafe { t_read(tap, buf.as_mut_ptr(), buf.len()) };
        if n <= 0 {
            continue;
        }
        if buf[..n as usize].iter().any(|&b| b != 0) {
            return true;
        }
    }
    false
}

/// A /dev/nocturne/audio READ must be refused (N-3c-1). The mode admits the
/// open (0o666, for playback tools that open O_RDWR), so the refusal lands on
/// the READ. True iff no byte of the sink mix leaked through the mount.
fn mount_audio_read_refused() -> bool {
    let ma = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, MOUNT_AUDIO.as_ptr(), MOUNT_AUDIO.len(), T_OREAD) };
    if ma < 0 {
        return true; // refused at open -- also acceptable
    }
    let mut buf = [0u8; 64];
    let n = unsafe { t_read(ma, buf.as_mut_ptr(), buf.len()) };
    unsafe { t_close(ma) };
    n < 0
}

/// The DENY child (a user principal): opening the -ctl tap AND reading the mount
/// audio must BOTH be refused. Exit 0 iff both were refused.
fn deny_child() -> i64 {
    if !mount_audio_read_refused() {
        say("NOCTURNE-TAP-DENY FAIL: user READ the sink through /dev/nocturne/audio (mount eavesdrop)\n");
        return 1;
    }
    let ctl = connect_ctl();
    if ctl < 0 {
        say("NOCTURNE-TAP-DENY FAIL: child could not connect /srv/nocturne-ctl\n");
        return 3;
    }
    let tap = open_tap(ctl);
    if tap >= 0 {
        unsafe { t_close(tap) };
        unsafe { t_close(ctl) };
        say("NOCTURNE-TAP-DENY FAIL: user OPENED the sink tap -- gate bypassed\n");
        return 1;
    }
    unsafe { t_close(ctl) };
    say("NOCTURNE-TAP-DENY OK: user tap open + mount audio read both refused\n");
    0
}

fn parent() -> i64 {
    // 1. SYSTEM opens the tap (the positive authority arm).
    let ctl = connect_ctl();
    if ctl < 0 {
        return fail("connect /srv/nocturne-ctl");
    }
    let tap = open_tap(ctl);
    if tap < 0 {
        unsafe { t_close(ctl) };
        return fail("SYSTEM tap open was refused");
    }

    // 2. Single-reader guard: a SECOND concurrent tap open must be EBUSY. A
    //    fresh -ctl conn (also SYSTEM, so authority is not what refuses it).
    let ctl2 = connect_ctl();
    if ctl2 < 0 {
        return fail("second connect /srv/nocturne-ctl");
    }
    let tap2 = open_tap(ctl2);
    if tap2 >= 0 {
        unsafe { t_close(tap2) };
        unsafe { t_close(ctl2) };
        return fail("a SECOND concurrent tap open was ACCEPTED (single-reader guard)");
    }
    unsafe { t_close(ctl2) };

    // 3. Play a tone and capture it on the tap (the positive capture arm -- the
    //    tap must be open BEFORE the tone so the mirror records it).
    if !play_tone() {
        unsafe { t_close(tap) };
        unsafe { t_close(ctl) };
        return fail("could not play the tone");
    }
    if !tap_captures_nonsilence(tap) {
        unsafe { t_close(tap) };
        unsafe { t_close(ctl) };
        return fail("the tap captured only silence while a tone played");
    }
    // Release the tap (frees the single-reader guard for the deny child's probe).
    unsafe { t_close(tap) };
    unsafe { t_close(ctl) };

    // 4. The eavesdrop-via-mount regression: a mount audio READ is refused.
    if !mount_audio_read_refused() {
        return fail("/dev/nocturne/audio READ returned sink data (eavesdrop via the mount)");
    }

    // 5. Negative gate: a user-principal child -- tap open AND mount read both
    //    refused. The parent holds CAP_SET_IDENTITY (joey's spawn mask) to stamp
    //    the child's principal; the child inherits this namespace (so it sees the
    //    mount + /srv) but not SYSTEM, not the console-owner session, no
    //    clearance. stderr -> a fresh pipe (joey wires only fd 0+1).
    match Command::new(SELF_BIN)
        .arg("deny")
        .identity(DENY_PRINCIPAL, DENY_PRINCIPAL, &[])
        .caps(0)
        .stderr(Stdio::Piped)
        .spawn()
    {
        Ok(mut child) => match child.wait() {
            Ok(st) if st.success() => {}
            Ok(_) => return fail("deny child reported the tap gate was bypassed"),
            Err(_) => return fail("deny child wait() failed"),
        },
        Err(_) => return fail("could not spawn the deny child (missing CAP_SET_IDENTITY?)"),
    }

    say("NOCTURNE-TAP-PROBE PASS (SYSTEM tap open + single-reader + tone captured; mount read + user tap deny)\n");
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
