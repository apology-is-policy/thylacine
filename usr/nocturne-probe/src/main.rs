// /nocturne-probe -- the N-1 tone probe (docs/NOCTURNE.md section 7, W-1).
//
// Writes 0.5 s of a 1 kHz sine, then 0.5 s of 2 kHz, then 0.2 s of silence to
// /dev/nocturne/audio (S16LE stereo 48 kHz, -12 dBFS), reads /dev/nocturne/info
// back, and prints one verdict line on stdout: joey's smoke helper matches
// "NOCTURNE-PROBE PASS". Under THYLACINE_AUDIODEV=wav the host's
// tools/audio-verdict.py finds exactly this signature in the capture -- the
// two tones in that order, preceded by silence -- which is the positive
// control (2 kHz lands at 2 kHz) and the negative control (the pre-probe
// region is silent) in one file.
//
// The tone table is exact: 1 kHz at 48 kHz is 48 samples per cycle, so a
// 48-entry sine indexed by k plays 1 kHz and indexed by 2k plays 2 kHz with no
// floating point in a no_std binary.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::io::Write;
use libthyla_rs::{t_close, t_open, t_putstr, t_read, t_write, T_OREAD, T_OWRITE, T_WALK_OPEN_FROM_ROOT};

const RATE: usize = 48_000;
const TONE_FRAMES: usize = RATE / 2; // 0.5 s
const TAIL_FRAMES: usize = RATE / 5; // 0.2 s
const CHUNK_FRAMES: usize = 2048; // 8 KiB of S16 stereo per write

/// round(8192 * sin(2*pi*k/48)), k = 0..47.
const SINE48: [i16; 48] = [
         0,   1069,   2120,   3135,   4096,   4987,   5793,   6499,
      7094,   7568,   7913,   8122,   8192,   8122,   7913,   7568,
      7094,   6499,   5793,   4987,   4096,   3135,   2120,   1069,
         0,  -1069,  -2120,  -3135,  -4096,  -4987,  -5793,  -6499,
     -7094,  -7568,  -7913,  -8122,  -8192,  -8122,  -7913,  -7568,
     -7094,  -6499,  -5793,  -4987,  -4096,  -3135,  -2120,  -1069,
];

fn emit(out: &mut Vec<u8>, step: usize, frames: usize, phase: &mut usize) {
    for _ in 0..frames {
        let v = SINE48[*phase % 48];
        let b = v.to_le_bytes();
        out.extend_from_slice(&b); // left
        out.extend_from_slice(&b); // right
        *phase = (*phase + step) % 48;
    }
}

fn say(s: &str) {
    let _ = t_putstr(s);
    let mut out = libthyla_rs::io::stdout();
    let _ = out.write_all(s.as_bytes());
}

fn fail(why: &str) -> i64 {
    let mut s = alloc::string::String::from("NOCTURNE-PROBE FAIL: ");
    s.push_str(why);
    s.push('\n');
    say(&s);
    1
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

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let path = b"/dev/nocturne/audio";
    let fd = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, path.as_ptr(), path.len(), T_OWRITE) };
    if fd < 0 {
        return fail("open /dev/nocturne/audio");
    }
    let mut phase = 0usize;
    let mut buf: Vec<u8> = Vec::with_capacity(CHUNK_FRAMES * 4);
    // (step, frames): 1 kHz, 2 kHz, silence (step 0 at phase 0 is the zero sample).
    let plan: [(usize, usize); 3] = [(1, TONE_FRAMES), (2, TONE_FRAMES), (0, TAIL_FRAMES)];
    for (step, mut frames) in plan {
        if step == 0 {
            phase = 0;
        }
        while frames > 0 {
            let n = frames.min(CHUNK_FRAMES);
            buf.clear();
            emit(&mut buf, step, n, &mut phase);
            if !write_all(fd, &buf) {
                let _ = unsafe { t_close(fd) };
                return fail("write to /dev/nocturne/audio");
            }
            frames -= n;
        }
    }
    let _ = unsafe { t_close(fd) };

    let ipath = b"/dev/nocturne/info";
    let ifd = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, ipath.as_ptr(), ipath.len(), T_OREAD) };
    if ifd < 0 {
        return fail("open /dev/nocturne/info");
    }
    let mut info = [0u8; 1024];
    let n = unsafe { t_read(ifd, info.as_mut_ptr(), info.len()) };
    let _ = unsafe { t_close(ifd) };
    if n <= 0 {
        return fail("read /dev/nocturne/info");
    }
    let text = core::str::from_utf8(&info[..n as usize]).unwrap_or("");
    let mut played: u64 = 0;
    for line in text.lines() {
        if let Some(v) = line.strip_prefix("periods-played ") {
            played = v.trim().parse().unwrap_or(0);
        }
    }
    let mut s = alloc::string::String::from("nocturne-probe: info:\n");
    s.push_str(text);
    let _ = t_putstr(&s);
    if played == 0 {
        return fail("periods-played is 0 after the writes were accepted");
    }
    say("NOCTURNE-PROBE PASS (1 kHz + 2 kHz + tail written; periods played)\n");
    0
}
