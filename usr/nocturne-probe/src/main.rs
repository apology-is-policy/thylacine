// /nocturne-probe -- the N-2a-1 mixing witness (docs/NOCTURNE.md section 7, W-1).
//
// N-1 proved one voice plays. N-2a-1 proves the graph core MIXES: this mints
// TWO voices through /dev/nocturne/nodes/new, plays a 1 kHz sine on one and a
// 2 kHz sine on the other SIMULTANEOUSLY (interleaved writes keep both FIFOs
// fed, so nocturned's mixer sums them into every device period), then a silent
// tail. The host's tools/audio-verdict.py --chord asserts BOTH tones are
// present in the SAME 20 ms windows -- the discriminating proof of mixing (a
// sequential 1 kHz-then-2 kHz capture FAILS the chord check, so the witness
// cannot be satisfied by two voices that merely played at different times).
//
// The tone table is exact: 1 kHz at 48 kHz is 48 samples/cycle, so a 48-entry
// sine indexed by k plays 1 kHz and by 2k plays 2 kHz with no floating point.
// CHUNK_FRAMES = 1920 = 48*40 = 24*80, so each chunk is a whole number of both
// cycles and reused chunks splice seamlessly (no phase jump, no splatter).
//
// The /dev/nocturne mount is joey's shared connection, so the voices this probe
// mints outlive it (owned by the mount conn, not the probe). That is correct
// for a boot smoke; a client wanting per-exit voice lifetime connects directly
// to /srv/nocturne (the libtapestry idiom) -- the SDL backend's path (N-2a-2).

#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::io::Write;
use libthyla_rs::{t_close, t_open, t_putstr, t_read, t_write, T_OREAD, T_OWRITE, T_WALK_OPEN_FROM_ROOT};

const CHUNK_FRAMES: usize = 1920; // 40 ms at 48 kHz; 48*40 = 24*80 -> seamless for both tones
const CHORD_CHUNKS: usize = 30; // ~1.2 s of the mixed chord
const TAIL_CHUNKS: usize = 8; // ~0.32 s of silence

/// round(8192 * sin(2*pi*k/48)), k = 0..47 (-12 dBFS peak; two of these sum to
/// at most -6 dBFS, well clear of clipping).
const SINE48: [i16; 48] = [
         0,   1069,   2120,   3135,   4096,   4987,   5793,   6499,
      7094,   7568,   7913,   8122,   8192,   8122,   7913,   7568,
      7094,   6499,   5793,   4987,   4096,   3135,   2120,   1069,
         0,  -1069,  -2120,  -3135,  -4096,  -4987,  -5793,  -6499,
     -7094,  -7568,  -7913,  -8122,  -8192,  -8122,  -7913,  -7568,
     -7094,  -6499,  -5793,  -4987,  -4096,  -3135,  -2120,  -1069,
];

fn say(s: &str) {
    let _ = t_putstr(s);
    let mut out = libthyla_rs::io::stdout();
    let _ = out.write_all(s.as_bytes());
}

fn fail(why: &str) -> i64 {
    let mut s = String::from("NOCTURNE-PROBE FAIL: ");
    s.push_str(why);
    s.push('\n');
    say(&s);
    1
}

/// One seamless chunk of a tone: `step` 1 = 1 kHz, 2 = 2 kHz, 0 = silence.
fn tone_chunk(step: usize) -> Vec<u8> {
    let mut out: Vec<u8> = Vec::with_capacity(CHUNK_FRAMES * 4);
    let mut phase = 0usize;
    for _ in 0..CHUNK_FRAMES {
        let v = if step == 0 { 0 } else { SINE48[phase % 48] };
        let b = v.to_le_bytes();
        out.extend_from_slice(&b); // left
        out.extend_from_slice(&b); // right
        phase = (phase + step) % 48;
    }
    out
}

fn write_all(fd: i64, data: &[u8]) -> bool {
    let mut off = 0usize;
    while off < data.len() {
        let n = unsafe { t_write(fd, data.as_ptr().add(off), data.len() - off) };
        if n <= 0 {
            return false;
        }
        off += n as usize;
    }
    true
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

fn open_voice_audio(id: u32) -> i64 {
    let path = alloc::format!("/dev/nocturne/nodes/{}/audio", id);
    let b = path.as_bytes();
    unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b.as_ptr(), b.len(), T_OWRITE) }
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
    let a = match mint_voice() {
        Some(id) => id,
        None => return fail("mint voice A (/dev/nocturne/nodes/new)"),
    };
    let b = match mint_voice() {
        Some(id) => id,
        None => return fail("mint voice B (/dev/nocturne/nodes/new)"),
    };
    if a == b {
        return fail("nodes/new returned the same id twice");
    }
    let fd_a = open_voice_audio(a);
    if fd_a < 0 {
        return fail("open voice A audio");
    }
    let fd_b = open_voice_audio(b);
    if fd_b < 0 {
        let _ = unsafe { t_close(fd_a) };
        return fail("open voice B audio");
    }

    let one_khz = tone_chunk(1);
    let two_khz = tone_chunk(2);
    let silence = tone_chunk(0);

    // Interleave: a chunk of 1 kHz to A, a chunk of 2 kHz to B, repeating. Each
    // write parks when its voice's FIFO is full, which paces both to realtime
    // and keeps both fed -- so the mixer sums 1 kHz + 2 kHz into every period.
    for _ in 0..CHORD_CHUNKS {
        if !write_all(fd_a, &one_khz) || !write_all(fd_b, &two_khz) {
            let _ = unsafe { t_close(fd_a) };
            let _ = unsafe { t_close(fd_b) };
            return fail("write the mixed chord");
        }
    }
    // A silent tail on both voices so the capture ends in verifiable silence.
    for _ in 0..TAIL_CHUNKS {
        let _ = write_all(fd_a, &silence);
        let _ = write_all(fd_b, &silence);
    }
    let _ = unsafe { t_close(fd_a) };
    let _ = unsafe { t_close(fd_b) };

    // Both voices must have taken the bytes, and the sink must have played.
    let ia = read_info(alloc::format!("/dev/nocturne/nodes/{}/info", a).as_bytes());
    let ib = read_info(alloc::format!("/dev/nocturne/nodes/{}/info", b).as_bytes());
    let root = match read_info(b"/dev/nocturne/info") {
        Some(t) => t,
        None => return fail("read /dev/nocturne/info"),
    };
    let want = (CHORD_CHUNKS * CHUNK_FRAMES * 4) as u64;
    let a_in = ia.as_deref().map(|t| field_u64(t, "bytes-in ")).unwrap_or(0);
    let b_in = ib.as_deref().map(|t| field_u64(t, "bytes-in ")).unwrap_or(0);
    let played = field_u64(&root, "periods-played ");

    let mut s = String::from("nocturne-probe: root info:\n");
    s.push_str(&root);
    if let Some(t) = &ia {
        s.push_str("voice A info:\n");
        s.push_str(t);
    }
    if let Some(t) = &ib {
        s.push_str("voice B info:\n");
        s.push_str(t);
    }
    let _ = t_putstr(&s);

    if a_in < want || b_in < want {
        return fail("a voice did not take all its bytes");
    }
    if played == 0 {
        return fail("periods-played is 0 after both voices were written");
    }
    say("NOCTURNE-PROBE PASS (2-voice mix: 1 kHz + 2 kHz mixed; periods played)\n");
    0
}
