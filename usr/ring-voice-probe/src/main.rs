// /ring-voice-probe -- the Nocturne zero-copy ring witness (docs/NOCTURNE.md
// section 6.5). Two phases, one PASS.
//
//   N-2b-1 (the SUBSTRATE): mint a voice, open its `data` leaf THROUGH THE MOUNT
//   (SYS_WEFT_MAP needs a dev9p fd -- a direct /srv/nocturne srvconn will not
//   do), SYS_WEFT_MAP it, and validate the shared Weft ring geometry (WEFT_MAGIC,
//   K slots, a K-period payload). Controls (a check that cannot fail proves
//   nothing, #245): (a) a SECOND map of the same fd is idempotent (same VA);
//   (b) mapping VOICE 0's `data` is REFUSED -- voice 0 is the world-shared byte
//   sink and has no private SPSC ring, so a positive here would mean the id==0
//   gate is dead.
//
//   N-2b-2a (the PERIOD PROTOCOL): mint a SECOND ring voice, then STREAM a chord
//   THROUGH THE RINGS -- 1 kHz into voice A, 2 kHz into voice B, in lockstep with
//   back-pressure (t_yield when a ring is full) so nocturned's mixer sums two
//   RING voices into every device period. The host-side tools/test-ring-audio.sh
//   captures the wav and tools/audio-verdict.py --chord asserts BOTH tones in the
//   SAME windows: the discriminating proof that the zero-copy path carried real
//   audio AND the mixer summed two ring voices (a sequential or single-tone
//   capture FAILS the chord check). The exclusive boot arm (joey runs THIS probe
//   instead of the byte /nocturne-probe under thylacine.ringprobe) makes the ring
//   the ONLY thing in the capture, so any chord present came through the ring.
//   Guest-side corroborators: dropped==0 on both rings (no period failed the
//   consumer's len validation) and periods-played>0 (the sink played).
//
// Both voices are minted through joey's shared /dev/nocturne mount, so they (and
// their rings) outlive the probe -- owned by the mount conn, not the probe (the
// F5 limitation, shared with /nocturne-probe). Benign for a boot witness.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::io::Write;
use libthyla_rs::weft;
use libthyla_rs::{
    t_close, t_open, t_putstr, t_read, t_weft_map, t_yield, T_OREAD, T_WALK_OPEN_FROM_ROOT,
};

// Must match nocturned/src/server.rs (RING_ENTRIES) and snd.rs (PERIOD_BYTES).
const RING_ENTRIES: u32 = 8;
const PERIOD_BYTES: usize = 2048;

// ~1.4 s of chord (512 frames/period at 48 kHz = 10.67 ms), then a silent tail
// (nocturned's idle-stop adds ~0.5 s more before it stops the stream). Clears
// audio-verdict's MIN_CHORD_WINDOWS (15) and MIN_TAIL_WINDOWS (10) comfortably.
const CHORD_PERIODS: usize = 130;
const TAIL_PERIODS: usize = 24;
// A deadlock backstop for the back-pressure loop: a live consumer frees a slot
// within ~1 period (10.67 ms) plus the <=100 ms stopped-stream start latency, so
// this many no-slot yields means the ring is wedged (the consumer is not
// draining), which FAILS the probe loudly rather than spinning forever.
const STALL_YIELD_BOUND: u32 = 2_000_000;

/// round(8192 * sin(2*pi*k/48)), k = 0..47 (-12 dBFS peak; two of these sum to
/// at most -6 dBFS, well clear of clipping). 1 kHz at 48 kHz is 48 samples/cycle,
/// so indexing by k plays 1 kHz and by 2k plays 2 kHz with no floating point.
const SINE48: [i16; 48] = [
    0, 1069, 2120, 3135, 4096, 4987, 5793, 6499, 7094, 7568, 7913, 8122, 8192, 8122, 7913, 7568,
    7094, 6499, 5793, 4987, 4096, 3135, 2120, 1069, 0, -1069, -2120, -3135, -4096, -4987, -5793,
    -6499, -7094, -7568, -7913, -8122, -8192, -8122, -7913, -7568, -7094, -6499, -5793, -4987,
    -4096, -3135, -2120, -1069,
];

fn say(s: &str) {
    let _ = t_putstr(s);
    let mut out = libthyla_rs::io::stdout();
    let _ = out.write_all(s.as_bytes());
}

fn fail(why: &str) -> i64 {
    let mut s = String::from("RING-VOICE-PROBE FAIL: ");
    s.push_str(why);
    s.push('\n');
    say(&s);
    1
}

/// Mint a voice via /dev/nocturne/nodes/new; the read returns its decimal id.
fn mint_voice() -> Option<u32> {
    let p = b"/dev/nocturne/nodes/new";
    let fd = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, p.as_ptr(), p.len(), T_OREAD) };
    if fd < 0 {
        return None;
    }
    let mut buf = [0u8; 16];
    let n = unsafe { t_read(fd, buf.as_mut_ptr(), buf.len()) };
    let _ = unsafe { t_close(fd) };
    if n <= 0 {
        return None;
    }
    core::str::from_utf8(&buf[..n as usize]).ok()?.trim().parse::<u32>().ok()
}

/// Open a voice's `data` map fid THROUGH THE MOUNT (the dev9p path SYS_WEFT_MAP
/// requires). The open mode is a formality -- the ring is mapped RW by the kernel
/// regardless; the fid is only the Tweft anchor.
fn open_data(id: u32) -> i64 {
    let path = alloc::format!("/dev/nocturne/nodes/{}/data", id);
    let b = path.as_bytes();
    unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b.as_ptr(), b.len(), T_OREAD) }
}

/// A mapped ring: the data fid (held so the mapping stays live) + its base VA and
/// geometry.
struct MappedRing {
    data_fd: i64,
    va: u64,
    geom: weft::RingGeom,
}

/// Mint + map one ring voice; validates WEFT_MAGIC + the K-period geometry.
fn map_ring_voice() -> Result<(u32, MappedRing), &'static str> {
    let id = mint_voice().ok_or("mint voice (/dev/nocturne/nodes/new)")?;
    let data_fd = open_data(id);
    if data_fd < 0 {
        return Err("open nodes/<id>/data through the mount");
    }
    let va = unsafe { t_weft_map(data_fd as u64, 0) };
    if va < 0 {
        let _ = unsafe { t_close(data_fd) };
        return Err("SYS_WEFT_MAP the voice ring");
    }
    let va = va as u64;
    let geom = match unsafe { weft::read_ring_geom(va as *const u8) } {
        Some(g) => g,
        None => {
            let _ = unsafe { t_close(data_fd) };
            return Err("read_ring_geom (bad WEFT_MAGIC / geometry)");
        }
    };
    if geom.ring_entries != RING_ENTRIES {
        return Err("ring_entries mismatch");
    }
    if geom.payload_size < RING_ENTRIES * PERIOD_BYTES as u32 {
        return Err("payload region too small for K periods");
    }
    Ok((id, MappedRing { data_fd, va, geom }))
}

/// Fill `buf` (PERIOD_BYTES, S16LE stereo) with a tone: step 1 = 1 kHz, 2 = 2 kHz,
/// 0 = silence. `phase` carries ACROSS periods so the tone is continuous (a period
/// is not a whole number of cycles -- 512 frames vs a 48-sample table).
fn fill_period(buf: &mut [u8], phase: &mut usize, step: usize) {
    let mut i = 0;
    while i + 4 <= buf.len() {
        let v = if step == 0 { 0 } else { SINE48[*phase % 48] };
        let b = v.to_le_bytes();
        buf[i] = b[0];
        buf[i + 1] = b[1]; // left
        buf[i + 2] = b[0];
        buf[i + 3] = b[1]; // right
        *phase = (*phase + step) % 48;
        i += 4;
    }
}

/// Publish ONE period into a ring, back-pressuring (yield + retry) while it is
/// full. Fails only if the consumer never drains within STALL_YIELD_BOUND yields.
fn produce_one(va: u64, geom: &weft::RingGeom, period: &[u8]) -> Result<(), &'static str> {
    let mut spins: u32 = 0;
    loop {
        if unsafe { weft::slot_produce(va as *mut u8, geom, PERIOD_BYTES as u32, period) } {
            return Ok(());
        }
        spins += 1;
        if spins >= STALL_YIELD_BOUND {
            return Err("ring full for too long: consumer not draining (wedged)");
        }
        let _ = t_yield();
    }
}

/// The consumer-written `dropped` counter (WeftRingHdr offset 28): periods whose
/// client-written len failed the consumer's validation. Must be 0 -- every period
/// this probe writes is a full, frame-aligned PERIOD_BYTES.
fn ring_dropped(va: u64) -> u32 {
    unsafe { core::ptr::read_volatile((va + 28) as *const u32) }
}

fn read_info(path: &[u8]) -> Option<String> {
    let fd = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, path.as_ptr(), path.len(), T_OREAD) };
    if fd < 0 {
        return None;
    }
    let mut info = [0u8; 1024];
    let n = unsafe { t_read(fd, info.as_mut_ptr(), info.len()) };
    let _ = unsafe { t_close(fd) };
    if n <= 0 {
        return None;
    }
    Some(String::from(core::str::from_utf8(&info[..n as usize]).unwrap_or("")))
}

fn field_u64(text: &str, key: &str) -> u64 {
    for line in text.lines() {
        if let Some(v) = line.strip_prefix(key) {
            return v.trim().parse().unwrap_or(0);
        }
    }
    0
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    // === N-2b-1: the substrate on voice A ===
    let (a, ring_a) = match map_ring_voice() {
        Ok(v) => v,
        Err(e) => return fail(e),
    };

    // Control (a): a second map of the same fd is idempotent (same VA).
    let va2 = unsafe { t_weft_map(ring_a.data_fd as u64, 0) };
    if va2 < 0 || va2 as u64 != ring_a.va {
        return fail("second map of the same fd is not idempotent");
    }

    // Control (b): voice 0's data map must be REFUSED (no world-shared ring).
    let d0 = open_data(0);
    if d0 >= 0 {
        let r0 = unsafe { t_weft_map(d0 as u64, 0) };
        let _ = unsafe { t_close(d0) };
        if r0 >= 0 {
            return fail("voice 0 data map was NOT refused (the id==0 gate is dead)");
        }
    }

    let s = alloc::format!(
        "ring-voice-probe: mapped ring va={:#x} K={} size={} payload={} (voice {})\n",
        ring_a.va, ring_a.geom.ring_entries, ring_a.geom.ring_size, ring_a.geom.payload_size, a,
    );
    say(&s);

    // === N-2b-2a: the period protocol -- stream a chord THROUGH the rings ===
    let (b, ring_b) = match map_ring_voice() {
        Ok(v) => v,
        Err(e) => return fail(e),
    };
    if a == b {
        return fail("nodes/new returned the same id twice");
    }

    let mut phase_a = 0usize;
    let mut phase_b = 0usize;
    let mut period = [0u8; PERIOD_BYTES];
    // Lockstep so both rings stay within one period of each other: the mixer then
    // sums 1 kHz + 2 kHz into every device period (the chord). Each produce_one
    // back-pressures to the device clock.
    for _ in 0..CHORD_PERIODS {
        fill_period(&mut period, &mut phase_a, 1);
        if let Err(e) = produce_one(ring_a.va, &ring_a.geom, &period) {
            return fail(e);
        }
        fill_period(&mut period, &mut phase_b, 2);
        if let Err(e) = produce_one(ring_b.va, &ring_b.geom, &period) {
            return fail(e);
        }
    }
    // A silent tail on both so the capture ends in verifiable silence.
    let silence = [0u8; PERIOD_BYTES];
    for _ in 0..TAIL_PERIODS {
        if produce_one(ring_a.va, &ring_a.geom, &silence).is_err()
            || produce_one(ring_b.va, &ring_b.geom, &silence).is_err()
        {
            return fail("silent tail: ring wedged");
        }
    }

    // Guest-side corroborators (the wav is the real witness): no period was
    // rejected by the consumer's len validation, and the sink played.
    let da = ring_dropped(ring_a.va);
    let db = ring_dropped(ring_b.va);
    if da != 0 || db != 0 {
        return fail(&alloc::format!("consumer dropped periods (A={}, B={})", da, db));
    }
    let root = match read_info(b"/dev/nocturne/info") {
        Some(t) => t,
        None => return fail("read /dev/nocturne/info"),
    };
    let _ = t_putstr(&root);
    if field_u64(&root, "periods-played ") == 0 {
        return fail("periods-played is 0 after the ring chord");
    }

    let _ = unsafe { t_close(ring_a.data_fd) };
    let _ = unsafe { t_close(ring_b.data_fd) };
    say("RING-VOICE-PROBE PASS (weft ring mapped + geometry valid + 2 controls + ring chord: 1 kHz + 2 kHz mixed on two ring voices; dropped=0)\n");
    0
}
