// /warp-prove -- the Warp-2 gate prover (GPU-DESIGN.md section 12 row 2):
// "contexts create/destroy; a hand-built command stream round-trips".
//
// Drives the warp seam end to end on a VIRGL host (the thyla-gl gate run;
// a 2D device answers `virgl 0` and the probe reports SKIP). It connects to
// /srv/warp DIRECTLY -- its own 9P conn, so the conn IS its identity (the
// F3 clause; the /dev/warp MOUNT rides the kernel's single shared srvconn,
// where every Proc would alias to one client) -- and the conn's death
// retires everything it minted, so a failed run leaks nothing.
//
//   1. mint a context (ctx/new) + a BO (bo/new); build its 64x64 B8G8R8A8
//      render-target backing (bo ctl create3d),
//   2. map the BO client-side (the Tweft map fid) and sentinel-fill it --
//      the readback assert must DISCRIMINATE (stale zeros or a no-op
//      transfer leave the sentinel, never the clear color),
//   3. submit a hand-built VIRGL CCMD stream (virgl_protocol.h encodings
//      pinned below): CREATE_SUB_CTX + SET_SUB_CTX + CREATE_OBJECT(SURFACE)
//      + SET_FRAMEBUFFER_STATE + CLEAR(red),
//   4. queue TRANSFER_FROM_HOST_3D (bo ctl transfer_from) -- the readback
//      into the BO backing,
//   5. read the fence FILE until the pixels land (each read parks until a
//      newer fence retires -- the W2d completion stream), then assert five
//      sample pixels carry the clear color 0xFFFF0000 (B8G8R8A8 red:
//      B=0 G=0 R=255 A=255, one little-endian u32),
//   6. destroy the ctx and assert the tree's live-ctx count returns to 0.
//
// Exit: 0 = PASS; 2 = SKIP (no virgl -- a plain 2D boot); 1 = FAIL.

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;
use libthyla_rs::{
    t_close, t_exits, t_open, t_putstr, t_read, t_weft_map, t_write, T_OREAD, T_OWRITE,
    T_WALK_OPEN_FROM_ROOT,
};

// virgl_protocol.h (virglrenderer; wire-frozen). VIRGL_CMD0 packs the
// stream header dword: cmd | obj<<8 | payload-dword-count<<16.
const VIRGL_CCMD_CREATE_OBJECT: u32 = 1;
const VIRGL_CCMD_SET_FRAMEBUFFER_STATE: u32 = 5;
const VIRGL_CCMD_CLEAR: u32 = 7;
// Counted from VIRGL_CCMD_NOP = 0 in the enum, NOT guessed. The first
// attempt used 21 and vrend answered `failed to dispatch GET_QUERY_RESULT`
// + `Illegal command buffer` -- and GET_QUERY_RESULT is exactly 21, so the
// runtime confirmed the counting rule while refuting the value.
const VIRGL_CCMD_BLIT: u32 = 16;
// 17 -- ONE PAST BLIT. Counted out of the enum, never grepped for: the
// server's first cut of this same constant read 96, a line number, and only
// the healthy control caught it.
const VIRGL_CCMD_RESOURCE_COPY_REGION: u32 = 17;
const VIRGL_CMD_RCR_SIZE: u32 = 13;
const VIRGL_CCMD_SET_SUB_CTX: u32 = 28;
const VIRGL_CCMD_CREATE_SUB_CTX: u32 = 29;
// virgl_protocol.h "blit": 21 payload dwords, S0 packing mask|filter|scissor.
const VIRGL_CMD_BLIT_SIZE: u32 = 21;
// util/format/u_formats.h -- R|G|B|A. A ZERO mask is a legal no-op blit, so
// getting this wrong would forge the exact negative this probe reports.
const PIPE_MASK_RGBA: u32 = 0xf;
const PIPE_TEX_FILTER_NEAREST: u32 = 0;
const VIRGL_OBJECT_SURFACE: u32 = 8;
// virgl_hw.h
const VIRGL_FORMAT_B8G8R8A8_UNORM: u32 = 1;
const VIRGL_BIND_RENDER_TARGET: u32 = 1 << 1;
// gallium p_defines.h values the virgl wire carries verbatim (vrend decodes
// them against the same constants; frozen with the protocol).
const PIPE_TEXTURE_2D: u32 = 2;
/// A BUFFER resource, as the server mints its probe pairs (`PIPE_BUFFER`,
/// one byte per texel in `R8_UNORM`, width = the byte length, a vertex-buffer
/// bind): the C0-F1 attacker's source since the C-0d Fable round, because the
/// probe under attack is a buffer pair now and a texture->buffer copy is not
/// a legal copy -- the leg would "defend" for the wrong reason.
const PIPE_BUFFER: u32 = 0;
const VIRGL_FORMAT_R8_UNORM: u32 = 64;
const VIRGL_BIND_VERTEX_BUFFER: u32 = 1 << 4;
const PROBE_BUF_BYTES: u32 = 4096;
const PIPE_CLEAR_COLOR0: u32 = 1 << 2;

const W: u32 = 64;
const H: u32 = 64;
const BO_SIZE: u32 = W * H * 4;
const SENTINEL: u32 = 0x4141_4141;
// B8G8R8A8 as one little-endian u32: byte0=B byte1=G byte2=R byte3=A.
// Chosen so all THREE outcomes of the C-0 P1 leg are distinguishable:
// SENTINEL = the readback never landed; RED = the destination's own clear
// survived (the blit moved nothing); GREEN = the blit moved the source.
const BLIT_RED: u32 = 0xFFFF_0000;
const BLIT_GREEN: u32 = 0xFF00_FF00;
// Not a pixel: `resample`'s report that the context wedged before the
// readback could land. Distinct from SENTINEL so "the submit was refused"
// never reads as "the instrument failed".
const CTX_POISONED: u32 = 0xDEAD_0001;
const XFER_REFUSED: u32 = 0xDEAD_0002;
const CLEAR_RED_B8G8R8A8: u32 = 0xFFFF_0000;

fn cmd0(cmd: u32, obj: u32, len: u32) -> u32 {
    cmd | (obj << 8) | (len << 16)
}

fn fail(msg: &str) -> ! {
    t_putstr("warp-prove: FAIL -- ");
    t_putstr(msg);
    t_putstr("\n");
    unsafe { t_exits(1) }
}

fn read_string(fd: i64) -> String {
    let mut buf = [0u8; 512];
    let n = unsafe { t_read(fd, buf.as_mut_ptr(), buf.len()) };
    if n <= 0 {
        return String::new();
    }
    String::from_utf8_lossy(&buf[..n as usize]).into_owned()
}

/// Read to EOF (round-7 F6). The 512-byte single read above IS the warp
/// ctl's documented snapshot contract; the TAPESTRY ctl has no such
/// discipline -- its 19 ever-growing cost rows pass 512 bytes on a
/// long-lived boot, and a row cut mid-number parses as a SMALLER,
/// arithmetically possible value (strictly worse than the impossible
/// splice this mode was built to fix).
fn read_string_all(fd: i64) -> String {
    let mut out = String::new();
    loop {
        let mut buf = [0u8; 512];
        let n = unsafe { t_read(fd, buf.as_mut_ptr(), buf.len()) };
        if n <= 0 {
            return out;
        }
        out.push_str(&String::from_utf8_lossy(&buf[..n as usize]));
        if (n as usize) < buf.len() {
            return out;
        }
    }
}

fn open_read_string(root: i64, path: &str) -> String {
    let fd = unsafe { t_open(root, path.as_ptr(), path.len(), T_OREAD) };
    if fd < 0 {
        fail("open-for-read");
    }
    let s = read_string(fd);
    unsafe { t_close(fd) };
    s
}

fn write_ctl(root: i64, path: &str, data: &str) -> bool {
    let fd = unsafe { t_open(root, path.as_ptr(), path.len(), T_OWRITE) };
    if fd < 0 {
        return false;
    }
    let n = unsafe { t_write(fd, data.as_ptr(), data.len()) };
    unsafe { t_close(fd) };
    n == data.len() as i64
}

/// The broker's stall (GPU-DESIGN 4.5.15) refuses every submission with EAGAIN until
/// the synchronous chain it waits on retires, and a deep queue of this program's own
/// can cause it; past FENCE_ABANDON_MS (30 s) the chain is abandoned, so a refusal
/// still standing after this bound is a fault, not a stall.
const STALL_RESEND_BOUND_MS: u64 = 35_000;

/// Write one whole command, resending it while the broker answers EAGAIN (its stall),
/// up to STALL_RESEND_BOUND_MS. Returns the write's last answer and how many times
/// the stall refused it; a refused command was never queued, so resending is exact.
/// Every stall refusal any send met, process-wide: a leg whose construction a
/// stall can defeat compares it across its sends and retries instead of judging.
static STALL_REFUSED: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(0);
fn stall_refusals() -> u32 {
    STALL_REFUSED.load(core::sync::atomic::Ordering::Relaxed)
}
fn write_behind_stall(root: i64, path: &str, bytes: &[u8]) -> (i64, u32) {
    let t0 = libthyla_rs::time::Instant::now();
    let mut refused = 0u32;
    loop {
        let fd = unsafe { t_open(root, path.as_ptr(), path.len(), T_OWRITE) };
        if fd < 0 {
            return (fd, refused);
        }
        let n = unsafe { t_write(fd, bytes.as_ptr(), bytes.len()) };
        unsafe { t_close(fd) };
        if n != -11 || t0.elapsed().as_millis() as u64 >= STALL_RESEND_BOUND_MS {
            return (n, refused);
        }
        refused += 1;
        STALL_REFUSED.fetch_add(1, core::sync::atomic::Ordering::Relaxed);
        let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(20));
    }
}

/// One write's return, a stall's EAGAIN resent within the bound; Err(rc) when
/// the open itself is refused, so a refusal is never read as the write's.
fn write_rc(root: i64, path: &str, bytes: &[u8]) -> Result<i64, i64> {
    let t0 = libthyla_rs::time::Instant::now();
    loop {
        let fd = unsafe { t_open(root, path.as_ptr(), path.len(), T_OWRITE) };
        if fd < 0 {
            return Err(fd);
        }
        let n = unsafe { t_write(fd, bytes.as_ptr(), bytes.len()) };
        unsafe { t_close(fd) };
        if n != -11 || t0.elapsed().as_millis() as u64 >= STALL_RESEND_BOUND_MS {
            return Ok(n);
        }
        let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(20));
    }
}

/// Say that the broker's stall refused a command before it was taken.
fn note_stall_refusals(what: &str, refused: u32) {
    if refused > 0 {
        t_putstr(&format!(
            "warp-prove: {} -- the broker's stall refused it {} times before it was taken\n",
            what, refused
        ));
    }
}

fn parse_u32_prefix(s: &str) -> Option<u32> {
    s.trim().split_ascii_whitespace().next()?.parse().ok()
}

/// Like `open_read_string` but survives an open REFUSAL -- for legs whose
/// assertion is about whether the open succeeds (a starved `bo/new` mint
/// refuses at the OPEN, and the generic "open-for-read" fail would leave a
/// red run blind to which leg and which attempt died).
fn try_open_read(root: i64, path: &str) -> Option<String> {
    let fd = unsafe { t_open(root, path.as_ptr(), path.len(), T_OREAD) };
    if fd < 0 {
        return None;
    }
    let s = read_string(fd);
    unsafe { t_close(fd) };
    Some(s)
}

/// Pull the decimal after `key ` out of a "k v k v ..." line.
fn parse_field(s: &str, key: &str) -> Option<u64> {
    let mut it = s.split_ascii_whitespace();
    while let Some(tok) = it.next() {
        if tok == key {
            return it.next()?.parse().ok();
        }
    }
    None
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    // #204 census mode: `warp-prove ctl` connects and dumps the global ctl
    // verbatim. The shell cannot cross the srv post (`cat /srv/warp/ctl`
    // fails "not a directory" -- a one-shot path walk cannot traverse a
    // post); this binary already owns the two-step attach, so it is the
    // census reader (bo-peak / fence-lane / bo-cap) for the glq lane.
    if libthyla_rs::env::args().get_str(1) == Some("ctl") {
        let root =
            unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv/warp".as_ptr(), 9, T_OREAD) };
        if root < 0 {
            t_putstr("warp-prove: ctl: open /srv/warp failed\n");
            unsafe { t_exits(1) };
        }
        let ctl = open_read_string(root, "ctl");
        t_putstr(&ctl);
        if !ctl.ends_with('\n') {
            t_putstr("\n");
        }
        unsafe { t_close(root) };
        return 0;
    }

    // W-4: the TAPESTRY-side census twin. The C-4 cost rows (`cost
    // present-poke-img` among them) live on the tapestry tree's ctl, not
    // warp's -- the vkq gate's poke census read the wrong ctl for three
    // runs and reported "unreadable" every time. Same two-step attach,
    // other root.
    //
    // An optional second arg FILTERS to rows containing it, printed in one
    // short write. The full dump streamed through the cons ring under
    // console pressure (aurora repainting at ~50 presents/s) SPLICED once:
    // the drop-OLDEST ring is unit-atomic per WRITE, so a mid-dump drop
    // glues two distant fragments into one plausible-looking row -- the
    // first poke census read back n=980 sum=20.45s max=318us, an
    // arithmetically impossible triple through cost_add's shared-ns feed.
    if libthyla_rs::env::args().get_str(1) == Some("tctl") {
        let root = unsafe {
            t_open(T_WALK_OPEN_FROM_ROOT, b"/srv/tapestry".as_ptr(), 13, T_OREAD)
        };
        if root < 0 {
            t_putstr("warp-prove: tctl: open /srv/tapestry failed\n");
            unsafe { t_exits(1) };
        }
        let cfd = unsafe { t_open(root, b"ctl".as_ptr(), 3, T_OREAD) };
        if cfd < 0 {
            t_putstr("warp-prove: tctl: open ctl failed\n");
            unsafe { t_exits(1) };
        }
        let ctl = read_string_all(cfd);
        unsafe { t_close(cfd) };
        unsafe { t_close(root) };
        if let Some(key) = libthyla_rs::env::args().get_str(2) {
            let mut out = String::new();
            for line in ctl.lines() {
                if line.contains(key) {
                    out.push_str(line);
                    out.push('\n');
                }
            }
            if out.is_empty() {
                out.push_str("tctl: no row matches\n");
            }
            t_putstr(&out);
            return 0;
        }
        t_putstr(&ctl);
        if !ctl.ends_with('\n') {
            t_putstr("\n");
        }
        return 0;
    }

    // #240 observation mode. Its own argv verb rather than a leg of the
    // battery: it spends 45 s waiting on a 30 s timeout, and the Warp-2
    // gate's whole value is being fast enough to run every time.
    if libthyla_rs::env::args().get_str(1) == Some("reject") {
        let probe = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv/warp".as_ptr(), 9, T_OREAD) };
        if probe < 0 {
            t_putstr("warp-prove: reject: open /srv/warp failed\n");
            unsafe { t_exits(1) };
        }
        let ctl = open_read_string(probe, "ctl");
        unsafe { t_close(probe) };
        if !ctl.starts_with("virgl 1") {
            t_putstr("warp-prove: C0-REJECT SKIP -- no virgl on this device\n");
            // Not a pass: the arms never ran (F6 -- DONE is a verdict).
            t_putstr("warp-prove: C0-REJECT INCOMPLETE(no-virgl)\n");
            return 0;
        }
        observe_rejection();
        return 0;
    }

    // Warp-C C-6 (GPU-DESIGN 4.5.13): the compositor readback arm under a
    // deep client queue. Its own verb for the same reason as `reject`: it
    // deliberately stalls the device for seconds, twice, and the Warp-2
    // gate must stay cheap.
    if libthyla_rs::env::args().get_str(1) == Some("readback") {
        let probe = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv/warp".as_ptr(), 9, T_OREAD) };
        if probe < 0 {
            t_putstr("warp-prove: readback: open /srv/warp failed\n");
            unsafe { t_exits(1) };
        }
        let ctl = open_read_string(probe, "ctl");
        unsafe { t_close(probe) };
        if !ctl.starts_with("virgl 1") {
            t_putstr("warp-prove: C6-READBACK SKIP -- no virgl on this device\n");
            t_putstr("warp-prove: C6-READBACK INCOMPLETE(no-virgl)\n");
            return 0;
        }
        observe_readback();
        return 0;
    }

    // A ctx kept alive with one poisoned fenced slot, so that a second client's
    // retirement can be timed against it (tools/warp/warp-poison-retire.exp).
    if libthyla_rs::env::args().get_str(1) == Some("poison-hold") {
        return poison_hold();
    }

    // A client rendering without pause, with a backlog on the device at any moment
    // (tools/interactive/ls-graphical-sak-chain.exp).
    if libthyla_rs::env::args().get_str(1) == Some("chain-hold") {
        return chain_hold();
    }

    // V-3a: the coherent-ring gate. Needs NO virgl (the ring is coherent
    // shmem + a doorbell), so it runs on the local 2D device.
    if libthyla_rs::env::args().get_str(1) == Some("ring") {
        return ring_prove();
    }

    // V-3b-2 (WARP-V3-DESIGN 0.12): the HOST3D ring witness. A host3d ring lives
    // under a capset-4 (venus) ctx, so this needs a VENUS device; it SKIPs on 2D
    // (no ctx) and on a virgl-without-venus device (the mint refuses).
    if libthyla_rs::env::args().get_str(1) == Some("ring-host3d") {
        return ring_host3d_prove();
    }

    // V-3b-2 cross-Proc E2E: the host3d-ring park->reclaim lifecycle + cross-conn
    // ring-ownership isolation. Venus-gated (SKIPs on 2D / virgl-without-venus).
    if libthyla_rs::env::args().get_str(1) == Some("ring-xproc") {
        return ring_xproc_prove();
    }

    // W-3c-1: the presentable ABI over the wire (walk/write/read/destroy/
    // re-register). Venus-gated like ring-host3d; the shape gate runs even on
    // the SKIP path, since it needs no venus.
    if libthyla_rs::env::args().get_str(1) == Some("img") {
        return img_prove();
    }
    if libthyla_rs::env::args().get_str(1) == Some("img-direct") {
        return img_direct_prove();
    }

    t_putstr("warp-prove: starting (the Warp-2 gate)\n");

    let root = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv/warp".as_ptr(), 9, T_OREAD) };
    if root < 0 {
        fail("open /srv/warp (is tapestryd serving?)");
    }

    let ctl = open_read_string(root, "ctl");
    if !ctl.starts_with("virgl 1") {
        t_putstr("warp-prove: SKIP -- no virgl on this device (ctl: ");
        t_putstr(ctl.lines().next().unwrap_or(""));
        t_putstr(")\n");
        unsafe { t_exits(2) }
    }

    // 1. Mint the context + the BO; build the render-target backing.
    let ctx = match parse_u32_prefix(&open_read_string(root, "ctx/new")) {
        Some(v) => v,
        None => fail("ctx/new mint"),
    };
    let bo = match parse_u32_prefix(&open_read_string(root, &format!("ctx/{}/bo/new", ctx))) {
        Some(v) => v,
        None => fail("bo/new mint"),
    };
    let bo_ctl = format!("ctx/{}/bo/{}/ctl", ctx, bo);
    let create = format!(
        "create3d {} {} {} {} {} 1 1 0 0 0 {}",
        PIPE_TEXTURE_2D, VIRGL_FORMAT_B8G8R8A8_UNORM, VIRGL_BIND_RENDER_TARGET, W, H, BO_SIZE
    );
    if !write_ctl(root, &bo_ctl, &create) {
        fail("bo create3d");
    }
    let info = open_read_string(root, &format!("ctx/{}/bo/{}/info", ctx, bo));
    let res_id = match parse_field(&info, "res") {
        Some(v) => v as u32,
        None => fail("bo info res id"),
    };

    // 2. Map the backing and sentinel-fill the sample points.
    let map_fd = unsafe {
        let p = format!("ctx/{}/bo/{}/map", ctx, bo);
        t_open(root, p.as_ptr(), p.len(), T_OREAD)
    };
    if map_fd < 0 {
        fail("bo map open");
    }
    let va = unsafe { t_weft_map(map_fd as u64, 0) };
    if va < 0 {
        fail("bo t_weft_map claim");
    }
    let px = |i: u32| -> *mut u32 { (va as u64 + (i as u64) * 4) as *mut u32 };
    let samples = [0, W - 1, (H - 1) * W, (H - 1) * W + (W - 1), (H / 2) * W + W / 2];
    for &s in &samples {
        unsafe { core::ptr::write_volatile(px(s), SENTINEL) };
    }

    // 3. The hand-built stream: sub-ctx 0 (Mesa always creates one; vrend
    // scopes state to it), a surface over the BO's resource, bind it as
    // cbuf0, clear to red (floats as IEEE bits; depth/stencil unused).
    let mut st: Vec<u32> = Vec::new();
    st.push(cmd0(VIRGL_CCMD_CREATE_SUB_CTX, 0, 1));
    st.push(0);
    st.push(cmd0(VIRGL_CCMD_SET_SUB_CTX, 0, 1));
    st.push(0);
    st.push(cmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, 5));
    st.push(1); // client-chosen surface handle
    st.push(res_id);
    st.push(VIRGL_FORMAT_B8G8R8A8_UNORM);
    st.push(0); // texture level
    st.push(0); // layers
    st.push(cmd0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3));
    st.push(1); // nr_cbufs
    st.push(0); // zsurf handle
    st.push(1); // cbuf0 = the surface handle
    st.push(cmd0(VIRGL_CCMD_CLEAR, 0, 8));
    st.push(PIPE_CLEAR_COLOR0);
    st.push(1.0f32.to_bits()); // r
    st.push(0.0f32.to_bits()); // g
    st.push(0.0f32.to_bits()); // b
    st.push(1.0f32.to_bits()); // a
    st.push(0); // depth (double) lo
    st.push(0); // depth hi
    st.push(0); // stencil
    let bytes: Vec<u8> = st.iter().flat_map(|d| d.to_le_bytes()).collect();

    let submit_fd = unsafe {
        let p = format!("ctx/{}/submit", ctx);
        t_open(root, p.as_ptr(), p.len(), T_OWRITE)
    };
    if submit_fd < 0 {
        fail("submit open");
    }
    let n = unsafe { t_write(submit_fd, bytes.as_ptr(), bytes.len()) };
    unsafe { t_close(submit_fd) };
    if n != bytes.len() as i64 {
        fail("submit write (queue refused)");
    }
    t_putstr("warp-prove: stream queued\n");

    // 4. The readback: device -> backing (fenced like the submit).
    let xfer = format!("transfer_from 0 0 0 0 {} {} 1 0 0 0", W, H);
    if !write_ctl(root, &bo_ctl, &xfer) {
        fail("transfer_from queue");
    }

    // 5. Ride the fence stream until the pixels land. Each read PARKS until
    // a newer fence retires; records coalesce, so the pixel check -- not a
    // record count -- is the loop condition. 8 reads >> the 2 fences issued.
    let fence_fd = unsafe {
        let p = format!("ctx/{}/fence", ctx);
        t_open(root, p.as_ptr(), p.len(), T_OREAD)
    };
    if fence_fd < 0 {
        fail("fence open");
    }
    let mut landed = false;
    for _ in 0..8 {
        let mut fb = [0u8; 64];
        let fn_ = unsafe { t_read(fence_fd, fb.as_mut_ptr(), fb.len()) };
        if fn_ <= 0 {
            fail("fence read (EOF/error)");
        }
        if unsafe { core::ptr::read_volatile(px(0)) } == CLEAR_RED_B8G8R8A8 {
            landed = true;
            break;
        }
    }
    unsafe { t_close(fence_fd) };
    if !landed {
        let got = unsafe { core::ptr::read_volatile(px(0)) };
        fail(&format!("pixel[0] never became the clear color (got {:#010x})", got));
    }
    for &s in &samples {
        let got = unsafe { core::ptr::read_volatile(px(s)) };
        if got != CLEAR_RED_B8G8R8A8 {
            fail(&format!("sample pixel {} = {:#010x} (want 0xffff0000)", s, got));
        }
    }
    t_putstr("warp-prove: clear color round-tripped (5 samples)\n");

    // 6. Destroy + the live-count assert (the create/destroy gate half).
    if !write_ctl(root, &format!("ctx/{}/ctl", ctx), "destroy") {
        fail("ctx destroy");
    }
    // `ctxs` alone is satisfiable by a DEFERRED destroy (round-5 F5):
    // warp_live_ctxs excludes `retiring` contexts by design, so a ctx that
    // could not quiesce -- and may still go on to leak every backing --
    // reads 0 immediately. Assert the thing deferral cannot fake first: the
    // ctx must be genuinely unresolvable.
    let gone = format!("ctx/{}/ctl", ctx);
    let re = unsafe { t_open(root, gone.as_ptr(), gone.len(), T_OREAD) };
    if re >= 0 {
        unsafe { t_close(re) };
        fail("ctx still resolvable after destroy");
    }
    let ctl2 = open_read_string(root, "ctl");
    match parse_field(&ctl2, "ctxs") {
        Some(0) => {}
        Some(v) => fail(&format!("ctxs {} after destroy (want 0)", v)),
        None => fail("ctl reread"),
    }
    // A condemned slot means a fence was abandoned somewhere in this run --
    // the gate must not pass with the seam in its wedge state.
    match parse_field(&ctl2, "poisoned") {
        Some(0) => {}
        Some(v) => fail(&format!("poisoned {} after a clean run (want 0)", v)),
        None => fail("ctl poisoned field missing"),
    }

    unsafe { t_close(map_fd) };

    // 7. The POISONED path (#175). Everything above is the clean path --
    // which is all this prover ever drove, and is why six consecutive audit
    // rounds each found another defect in the poison/graveyard/vindication
    // machine with nothing able to regress-test it.
    prove_poisoned_path(root);

    // 7b. The PROBE GRAVEYARD (#240 audit F3). The leg above drives the
    // wedge on a ctx that SURVIVES it; this one drives the wedge into a
    // ctx DESTROY, which is the only path that parks a probe -- and the
    // path that leaked its two handles forever until F3.
    prove_probe_reclaim(root);

    // 8. The TWO-CLIENT path (#180). Everything above -- including the
    // poisoned path -- drives ONE connection, so every CROSS-client property
    // of the seam was validated by reading alone. Round-8 F1 lived exactly
    // there: a hold that was scoped in effect but global in storage.
    prove_two_clients();

    // 9. The CORPSE-RECLAIM path (#218). Every leg above drives create3d
    // to SUCCEED (or, in the churn, to be refused exactly once at the
    // cap); none ever asked what a refusal leaves behind. It left the
    // minted-but-unbuilt record in `bos[]` forever -- ~cap per-texture
    // refusals starved the mint and converted a recoverable format
    // fallback into total BO death (the #198 cascade's second stage).
    prove_corpse_reclaim(root);

    // 10. Warp-C C-0 / P1 (GPU-DESIGN section 4.5.4): the cross-context
    // blit question the whole GPU-composition design rests on. Reports a
    // RESULT either way -- only a broken instrument fails the gate.
    prove_cross_ctx_blit();

    unsafe { t_close(root) };
    t_putstr("WARP-PROVE PASS (ctx create/destroy + CCMD round-trip + poisoned path + probe-reclaim + two-client + corpse-reclaim + C0-P1 cross-ctx blit)\n");
    unsafe { t_exits(0) }
}

/// Open a fresh /srv/warp CONNECTION.
///
/// Each open is its own connect, not a second handle on a shared session:
/// `SYS_OPEN` on a `/srv/<name>` leaf lands in `devsrv_open_connect`
/// (kernel/devsrv.c), which calls `srvconn_create` per open. tapestryd keys
/// context ownership on `owner_conn`, so two roots in ONE process are two
/// clients -- which is what makes this harness sequential and deterministic
/// instead of a race between two spawned processes.
// === V-3a: the coherent-ring gate (`warp-prove ring`) ======================
// The ring blob control-header offsets (must match server.rs WARP_RING_OFF_*).
const R_HEAD: u64 = 0x00;
const R_IDLE: u64 = 0x10;
const R_SEQ: u64 = 0x18;
const R_HDR: u64 = 0x40;

unsafe fn rld(va: u64, off: u64) -> u64 {
    core::ptr::read_volatile((va + off) as *const u64)
}
unsafe fn rst(va: u64, off: u64, v: u64) {
    core::ptr::write_volatile((va + off) as *mut u64, v);
}

fn ring_fail(msg: &str) -> ! {
    t_putstr("WARP-RING FAIL -- ");
    t_putstr(msg);
    t_putstr("\n");
    unsafe { t_exits(1) }
}

fn ring_prove() -> i64 {
    t_putstr("warp-prove: ring gate (V-3a) starting\n");
    let root = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv/warp".as_ptr(), 9, T_OREAD) };
    if root < 0 {
        ring_fail("open /srv/warp (is tapestryd serving?)");
    }
    t_putstr("warp-prove: ring connected; minting ctx (needs a virgl device)\n");
    let ctx = match try_open_read(root, "ctx/new").and_then(|s| parse_u32_prefix(&s)) {
        Some(v) => v,
        None => {
            // No virgl on this device: ctx creation is not available, so the
            // ring (which lives under a ctx) cannot be minted here. This is
            // the 2D-local case; the gate runs on a virgl device (the GL host).
            t_putstr("warp-prove: RING SKIP -- no virgl on this device (ctx mint unavailable)\n");
            unsafe { t_exits(2) }
        }
    };
    t_putstr("warp-prove: ring ctx minted\n");

    // 1. Mint a 4096-byte ring for ring_idx 0; check its info.
    if !write_ctl(root, &format!("ctx/{}/ring/new", ctx), "4096 0") {
        ring_fail("ring/new mint (4096 0)");
    }
    let info = open_read_string(root, &format!("ctx/{}/ring/0/info", ctx));
    if parse_field(&info, "bytes") != Some(4096) {
        ring_fail("ring info bytes != 4096");
    }
    if parse_field(&info, "ridx") != Some(0) {
        ring_fail("ring info ridx != 0");
    }
    if parse_field(&info, "hdr").is_none() {
        ring_fail("ring info missing hdr");
    }

    // 2. Map the ring; the host starts idle, seq 0.
    let map_fd = unsafe {
        let pth = format!("ctx/{}/ring/0/map", ctx);
        t_open(root, pth.as_ptr(), pth.len(), T_OREAD)
    };
    if map_fd < 0 {
        ring_fail("ring/0/map open");
    }
    let va = unsafe { t_weft_map(map_fd as u64, 0) };
    if va < 0 {
        ring_fail("ring t_weft_map claim");
    }
    let va = va as u64;
    if unsafe { rld(va, R_IDLE) } != 1 {
        ring_fail("ring idle != 1 at mint (host should start parked)");
    }
    if unsafe { rld(va, R_SEQ) } != 0 {
        ring_fail("ring seq != 0 at mint");
    }

    // 3. Doorbell: advance head (a CS marker), read idle, kick.
    unsafe { rst(va, R_HEAD, R_HDR + 16) };
    if unsafe { rld(va, R_IDLE) } != 1 {
        ring_fail("ring idle flipped before kick");
    }
    if !write_ctl(root, &format!("ctx/{}/ring/0/kick", ctx), "1") {
        ring_fail("ring/0/kick");
    }
    // 4a. Feedback-slot poll (zero-syscall). 4b. The blocking fence agrees.
    if unsafe { rld(va, R_SEQ) } != 1 {
        ring_fail("ring feedback seq != 1 after kick");
    }
    let fseq = match parse_u32_prefix(&open_read_string(root, &format!("ctx/{}/ring/0/fence", ctx))) {
        Some(v) => v,
        None => ring_fail("ring/0/fence read"),
    };
    if fseq != 1 {
        ring_fail("ring fence file seq != 1");
    }
    t_putstr("warp-prove: ring round-trip OK (map + doorbell + feedback + fence)\n");

    // 5. F2 rejection legs -- refused, never clamped.
    if write_ctl(root, &format!("ctx/{}/ring/new", ctx), "0 1") {
        ring_fail("F2: zero-byte ring accepted");
    }
    if write_ctl(root, &format!("ctx/{}/ring/new", ctx), "5000 2") {
        ring_fail("F2: unaligned ring accepted");
    }
    if write_ctl(root, &format!("ctx/{}/ring/new", ctx), "2097152 3") {
        ring_fail("F2: over-max ring accepted (WARP_RING_MAX is 1 MiB)");
    }
    if write_ctl(root, &format!("ctx/{}/ring/new", ctx), "4096 64") {
        ring_fail("F2: ring_idx 64 accepted (out of range)");
    }
    if write_ctl(root, &format!("ctx/{}/ring/new", ctx), "4096 0") {
        ring_fail("F2: duplicate ring_idx 0 accepted");
    }
    t_putstr("warp-prove: ring F2 rejections OK\n");

    // 6. I-45: OWNERSHIP, not liveness (audit F4). A SECOND conn mints a LIVE
    // ctx + ring; this conn must not resolve it -- the ownership gate tested
    // with the positive control (a live foreign ring) one variable away, so a
    // regression that ignored owner_conn would be caught. The non-existent-ctx
    // (liveness) leg is retained as a second, weaker check.
    let conn2 = warp_connect("i45-owner");
    let ctx2 = mint_ctx(conn2, "i45-owner");
    if !write_ctl(conn2, &format!("ctx/{}/ring/new", ctx2), "4096 0") {
        ring_fail("I-45 setup: conn2 ring/new mint");
    }
    if write_ctl(root, &format!("ctx/{}/ring/new", ctx2), "4096 1") {
        ring_fail("I-45: minted a ring under a FOREIGN-owned ctx");
    }
    if try_open_read(root, &format!("ctx/{}/ring/0/info", ctx2)).is_some() {
        ring_fail("I-45: read a FOREIGN-owned ring's info");
    }
    let alien = ctx2.wrapping_add(1000);
    if write_ctl(root, &format!("ctx/{}/ring/new", alien), "4096 0") {
        ring_fail("I-45: ring mint under a non-existent ctx accepted");
    }
    unsafe { t_close(conn2) };
    t_putstr("warp-prove: ring I-45 ownership + liveness gate OK\n");

    // 7. I-9 re-scan discrimination (WARP-V3-DESIGN 3.5): an armed mid-drain
    // inject models a guest advancing head in the idle-publish window.
    // Positive: with re-scan (default), it produces an extra completion.
    let base = unsafe { rld(va, R_SEQ) };
    if !write_ctl(root, "ctl", "ring-inject 0") {
        ring_fail("ring-inject arm");
    }
    if !write_ctl(root, &format!("ctx/{}/ring/0/kick", ctx), "1") {
        ring_fail("ring kick (inject, re-scan)");
    }
    let after_pos = unsafe { rld(va, R_SEQ) };
    if after_pos != base + 1 {
        ring_fail("I-9: re-scan did not deliver the injected advance");
    }
    // Negative (buggy arm): with noscan, the same inject is LOST.
    if !write_ctl(root, "ctl", "ring-noscan 0 on") {
        ring_fail("ring-noscan 0 on");
    }
    if !write_ctl(root, "ctl", "ring-inject 0") {
        ring_fail("ring-inject arm (noscan)");
    }
    if !write_ctl(root, &format!("ctx/{}/ring/0/kick", ctx), "1") {
        ring_fail("ring kick (inject, noscan)");
    }
    let after_neg = unsafe { rld(va, R_SEQ) };
    if after_neg != after_pos {
        ring_fail("I-9: noscan still delivered -- the test does NOT discriminate");
    }
    // Recovery: re-enable the re-scan; the stranded advance drains.
    if !write_ctl(root, "ctl", "ring-noscan 0 off") {
        ring_fail("ring-noscan 0 off");
    }
    if !write_ctl(root, &format!("ctx/{}/ring/0/kick", ctx), "1") {
        ring_fail("ring kick (recover)");
    }
    if unsafe { rld(va, R_SEQ) } != after_neg + 1 {
        ring_fail("I-9: re-enabled re-scan did not recover the stranded advance");
    }
    t_putstr("warp-prove: ring I-9 re-scan discrimination OK (delivered / lost / recovered)\n");

    // 8. Audit round-2 F1: the per-kick drain is BOUNDED. `head` is client-
    //    writable shared memory, so a multi-threaded client can advance it
    //    faster than the single serve thread drains and pin it forever (a
    //    box-wide DoS). A large ring + a multi-advance inject (count > the
    //    server's per-kick cap) drives ONE kick's drain past the cap; the fix
    //    caps it and yields (idle republished, the guest re-kicks), so no one
    //    kick monopolizes. Without the cap, one kick drains all `flood`
    //    advances. The single-threaded prover cannot build real client
    //    concurrency, so the multi-advance inject stands in for it -- same
    //    drain loop, same bound.
    // COUPLING (round-3 F2): big + flood are tied to the server-private cap
    // WARP_RING_MAX_DRAIN_PER_KICK (4096, not visible here). Discrimination needs
    // flood > cap (one capped kick drains < flood) AND big/WARP_RING_HDR(64) >
    // flood (so the min(tail+HDR, size) clamp never truncates the advance count).
    // If the server cap changes, raise both here or the leg fails misleadingly
    // ("not bounded"); server.rs pins this assumption at the const.
    let big: u64 = 512 * 1024; // 8192 advance-slots, > flood
    let flood: u64 = 5000; // > the server drain cap (WARP_RING_MAX_DRAIN_PER_KICK = 4096)
    if !write_ctl(root, &format!("ctx/{}/ring/new", ctx), &format!("{} 1", big)) {
        ring_fail("F1: large ring/1 mint");
    }
    let map_fd1 = unsafe {
        let pth = format!("ctx/{}/ring/1/map", ctx);
        t_open(root, pth.as_ptr(), pth.len(), T_OREAD)
    };
    if map_fd1 < 0 {
        ring_fail("F1: ring/1/map open");
    }
    let va1 = unsafe { t_weft_map(map_fd1 as u64, 0) };
    if va1 < 0 {
        ring_fail("F1: ring/1 weft_map");
    }
    let va1 = va1 as u64;
    let f1base = unsafe { rld(va1, R_SEQ) };
    if !write_ctl(root, "ctl", &format!("ring-inject 1 {}", flood)) {
        ring_fail("F1: ring-inject arm (count)");
    }
    if !write_ctl(root, &format!("ctx/{}/ring/1/kick", ctx), "1") {
        ring_fail("F1: first kick");
    }
    let delta1 = unsafe { rld(va1, R_SEQ) } - f1base;
    if delta1 == 0 || delta1 >= flood {
        ring_fail("F1: one kick was NOT bounded (need 0 < delta < flood)");
    }
    // The cap DEFERS work, it must not DROP it: re-kick until stable, then
    // assert the full `flood` eventually drained.
    let mut guard = 0;
    loop {
        let before = unsafe { rld(va1, R_SEQ) };
        if !write_ctl(root, &format!("ctx/{}/ring/1/kick", ctx), "1") {
            ring_fail("F1: drain kick");
        }
        if unsafe { rld(va1, R_SEQ) } == before {
            break; // stable -- all advances consumed
        }
        guard += 1;
        if guard > 16 {
            ring_fail("F1: drain did not converge in 16 kicks");
        }
    }
    if unsafe { rld(va1, R_SEQ) } - f1base != flood {
        ring_fail("F1: the cap dropped work (total drained != flood)");
    }
    unsafe { t_close(map_fd1) };
    t_putstr("warp-prove: ring F1 drain-cap bound OK (one kick capped; no work lost)\n");

    // Cleanup: retire the ctx (its rings tear down with it).
    if !write_ctl(root, &format!("ctx/{}/ctl", ctx), "destroy") {
        ring_fail("ctx destroy");
    }
    unsafe { t_close(map_fd) };
    unsafe { t_close(root) };
    t_putstr("WARP-RING PASS (transport + doorbell + feedback + fence + F2 + I-45 + I-9 re-scan + F1 drain-cap)\n");
    0
}

// === V-3b-2: the HOST3D ring witness (`warp-prove ring-host3d`) =============
// The Venus SUBMIT_CMD forward proof (WARP-V3-DESIGN 0.12). Mint a HOST3D ring,
// submit a hand-built vkCreateRingMESA naming the ring's virtio-gpu res_id, and
// observe virglrenderer set status&IDLE -- proof the host mapped the shmem and
// runs its poll thread, with NO Mesa. The bytes are venus-protocol e94b12f3
// (Mesa main's pin), byte-verified against Mesa's generated
// vn_encode_vkCreateRingMESA; the witness mechanism against virglrenderer
// vkr_ring.c:270-278 (poll thread sets IDLE) + vkr_ring.c:53 (create requires
// *head==0 && *status==0).

// The Venus ring layout (vn_ring.c), DISTINCT from the V-3a WARP_RING_OFF_*:
// head@0 / tail@64 / status@128 (each a bare u32 on its own 64-byte line),
// buffer@192 (128 KiB pow2), extra@192+128KiB (4B). The tight region is 131268;
// the mint page-aligns up from it. size (declared to the host) is the tight
// region -- Mesa-consistent and <= the page-rounded blob resource.
const VN_STATUS: u64 = 128;
const VN_BUFFER: u64 = 192;
const VN_BUFFER_SIZE: u64 = 128 * 1024;
const VN_EXTRA_SIZE: u64 = 4;
const VN_RING_TOTAL: u64 = VN_BUFFER + VN_BUFFER_SIZE + VN_EXTRA_SIZE; // 131268
const RING_PAGE: u64 = 4096;

// venus-protocol e94b12f3 (vn_protocol_driver_defines.h).
const VK_CMD_CREATE_RING: u32 = 188;
const VK_STYPE_RING_CREATE_INFO: u32 = 1000384000;
// virglrenderer vkr_ring.c status bits (VK_MESA_venus_protocol.xml:138-142).
const VK_RING_STATUS_IDLE: u32 = 0x1;
const VK_RING_STATUS_FATAL: u32 = 0x2;

unsafe fn rld32(va: u64, off: u64) -> u32 {
    core::ptr::read_volatile((va + off) as *const u32)
}

// A fixed encode buffer for the 124-byte bare (NULL-pNext) command.
struct RingCmd {
    b: [u8; 128],
    n: usize,
}
impl RingCmd {
    fn new() -> Self {
        RingCmd { b: [0u8; 128], n: 0 }
    }
    fn w32(&mut self, v: u32) {
        self.b[self.n..self.n + 4].copy_from_slice(&v.to_le_bytes());
        self.n += 4;
    }
    fn w64(&mut self, v: u64) {
        self.b[self.n..self.n + 8].copy_from_slice(&v.to_le_bytes());
        self.n += 8;
    }
}

/// Encode a bare (NULL-pNext) vkCreateRingMESA SUBMIT_CMD for a ring at
/// virtio-gpu `res_id`, whole-region `size`, `idle_ns` idleTimeout. Byte layout
/// per Mesa's generated vn_encode_vkCreateRingMESA: framing [cmd_type=188]
/// [cmd_flags=0][ring u64][pCreateInfo present u64=1], then VkRingCreateInfoMESA
/// {sType, pNext=NULL(8B 0), flags, resourceId, offset, size, idleTimeout,
/// headOffset, tailOffset, statusOffset, bufferOffset, bufferSize, extraOffset,
/// extraSize}. All words host-LE; size_t fields are 8 bytes on the wire. The
/// bare form is accepted by vkr_dispatch_vkCreateRingMESA (the monitor pNext
/// only starts the separate ALIVE watchdog, unneeded here). Total = 124 bytes.
fn encode_vk_create_ring(res_id: u32, size: u64, idle_ns: u64) -> RingCmd {
    let mut c = RingCmd::new();
    c.w32(VK_CMD_CREATE_RING); // cmd_type
    c.w32(0); // cmd_flags (0 = async, no reply)
    c.w64(0xDEAD_BEEF); // ring handle cookie (any unique u64)
    c.w64(1); // pCreateInfo present marker
    c.w32(VK_STYPE_RING_CREATE_INFO); // sType
    c.w64(0); // pNext = NULL
    c.w32(0); // flags
    c.w32(res_id); // resourceId
    c.w64(0); // offset
    c.w64(size); // size (whole region)
    c.w64(idle_ns); // idleTimeout (ns)
    c.w64(0); // headOffset
    c.w64(64); // tailOffset
    c.w64(VN_STATUS); // statusOffset
    c.w64(VN_BUFFER); // bufferOffset
    c.w64(VN_BUFFER_SIZE); // bufferSize (pow2)
    c.w64(VN_BUFFER + VN_BUFFER_SIZE); // extraOffset
    c.w64(VN_EXTRA_SIZE); // extraSize
    c
}

fn ring_host3d_fail(msg: &str) -> ! {
    t_putstr("WARP-RING-HOST3D FAIL -- ");
    t_putstr(msg);
    t_putstr("\n");
    unsafe { t_exits(1) }
}

fn img_fail(msg: &str) -> ! {
    t_putstr(&format!("warp-prove: IMG FAIL -- {}\n", msg));
    unsafe { t_exits(1) }
}

/// W-3c-1: the PRESENTABLE gate -- drives the `img/` ABI over the WIRE.
///
/// The compositor's own boot self-test exercises the presentable's INTERNALS
/// (mint, bind, the display-safe teardown) but reaches none of them through
/// 9P. This leg is the ABI's driver: walk, write, read-back, destroy,
/// re-register. Without it the whole namespace surface -- the img dir, the
/// info file, the ctl verb, the handle-frees-on-destroy contract -- would
/// have no caller until mesa arrives at W-3d, and a gate with no driver is
/// not a gate (the "BOOT OK does not prove a gate is wired" rule).
///
/// Venus-gated: a presentable is a venus-ctx object, so this SKIPs on a 2D
/// device (no ctx) and on virgl-without-venus (the mint refuses), exactly
/// like its ring-host3d sibling.
fn img_prove() -> i64 {
    t_putstr("warp-prove: img gate (W-3c-1, the presentable ABI) starting\n");
    let root = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv/warp".as_ptr(), 9, T_OREAD) };
    if root < 0 {
        img_fail("open /srv/warp (is tapestryd serving?)");
    }
    let ctx = match try_open_read(root, "ctx/new").and_then(|s| parse_u32_prefix(&s)) {
        Some(v) => v,
        None => {
            t_putstr("warp-prove: IMG SKIP -- no virgl on this device (ctx mint unavailable)\n");
            unsafe { t_exits(2) }
        }
    };
    const W: u32 = 64;
    const H: u32 = 64;
    const FMT: u32 = VIRGL_FORMAT_B8G8R8A8_UNORM; // == VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM
    const STRIDE: u32 = W * 4;
    let newp = format!("ctx/{}/img/new", ctx);
    let good = format!("0 {} {} {} {} 0", W, H, FMT, STRIDE);

    // NEGATIVES FIRST, and through the WIRE rather than the internals: three
    // declarations one variable away from the accepted one. They must all be
    // refused BEFORE the positive runs, so a device that refuses everything
    // cannot read as a pass -- the positive below is what separates the two.
    // u32::MAX, not FMT+1: the neighbouring virtio format is XRGB8, which the
    // ratified stage-0 accept set ADMITS (audit F7). A negative control must
    // sit outside every accept set the design may legitimately grow into, or
    // it pins a decision rather than testing a gate.
    let n_fmt = !write_ctl(root, &newp, &format!("0 {} {} {} {} 0", W, H, u32::MAX, STRIDE));
    let n_stride = !write_ctl(root, &newp, &format!("0 {} {} {} {} 0", W, H, FMT, STRIDE - 4));
    let n_dim = !write_ctl(root, &newp, &format!("0 0 {} {} {} 0", H, FMT, STRIDE));
    // A handle past the row must be refused too -- the slot-bound gate, which
    // no other arm touches.
    let n_handle = !write_ctl(root, &newp, &format!("9999 {} {} {} {} 0", W, H, FMT, STRIDE));
    if !(n_fmt && n_stride && n_dim && n_handle) {
        img_fail(&format!(
            "a malformed registration was ACCEPTED (fmt-refused={} stride-refused={} dim-refused={} handle-refused={})",
            n_fmt, n_stride, n_dim, n_handle
        ));
    }

    // The positive. A virgl-without-venus device refuses the venus-ctx create
    // -> SKIP (the shape gate above already ran, so the skip still carries
    // its result).
    if !write_ctl(root, &newp, &good) {
        t_putstr("warp-prove: IMG SKIP -- presentable mint refused (no venus device); shape gate PASSED\n");
        unsafe { t_exits(2) }
    }

    // The registration's RECORD: info must echo the shape that was declared,
    // which is what makes "the declaration is the negotiation" a checkable
    // claim rather than a slogan.
    let info = open_read_string(root, &format!("ctx/{}/img/0/info", ctx));
    let res = parse_field(&info, "res").unwrap_or_else(|| img_fail("img info missing res"));
    let gw = parse_field(&info, "w").unwrap_or_else(|| img_fail("img info missing w"));
    let gh = parse_field(&info, "h").unwrap_or_else(|| img_fail("img info missing h"));
    let gs = parse_field(&info, "stride").unwrap_or_else(|| img_fail("img info missing stride"));
    let gsz = parse_field(&info, "size").unwrap_or_else(|| img_fail("img info missing size"));
    let gb = parse_field(&info, "bound").unwrap_or_else(|| img_fail("img info missing bound"));
    // round-2 F10: `format` and `mem` were reported and read by NOTHING.
    // `mem` in particular is the client's only way to confirm the compositor
    // bound the allocation it MEANT, which is the whole point of echoing it.
    let gf = parse_field(&info, "format").unwrap_or_else(|| img_fail("img info missing format"));
    let gm = parse_field(&info, "mem").unwrap_or_else(|| img_fail("img info missing mem"));
    if gf != FMT as u64 {
        img_fail(&format!("img info format {} != declared {}", gf, FMT));
    }
    if gm != 0 {
        img_fail(&format!("img info mem {} != declared 0", gm));
    }
    if res == 0 {
        img_fail("img info res is 0 (no host resource)");
    }
    if gw != W as u64 || gh != H as u64 || gs != STRIDE as u64 {
        img_fail(&format!("img info shape {}x{} stride {} != declared {}x{} stride {}", gw, gh, gs, W, H, STRIDE));
    }
    if gsz != (STRIDE as u64) * (H as u64) {
        img_fail(&format!("img info size {} != stride*h {}", gsz, (STRIDE as u64) * (H as u64)));
    }
    if gb != 0 {
        img_fail("a freshly-registered presentable reports bound != 0");
    }

    // A DUPLICATE handle must be refused while the slot is live -- and it must
    // be refused for being taken, not for anything about the shape (the same
    // declaration that just succeeded).
    if write_ctl(root, &newp, &good) {
        img_fail("re-registering a LIVE handle was accepted (the slot is not exclusive)");
    }

    // The cross-conn I-45 leg (round-2 F10's one undriven property): a
    // SECOND connection must not resolve this conn's img -- not its info,
    // and not a consent naming it. Which layer refuses (the walk or the
    // verb) is not the property; that NOTHING is granted is.
    let alien = warp_connect("img-xproc");
    if try_open_read(alien, &format!("ctx/{}/img/0/info", ctx)).is_some() {
        img_fail("a FOREIGN conn read another client's img info (I-45)");
    }
    if write_ctl(alien, &format!("ctx/{}/ctl", ctx), "present-to 0 img 0") {
        img_fail("a FOREIGN conn consented another client's img to display (I-45)");
    }
    // And the probe must not have DAMAGED the owner's object (the #250
    // shape: a gate that mutates the fixture it shares).
    if try_open_read(root, &format!("ctx/{}/img/0/info", ctx)).is_none() {
        img_fail("the alien-conn probe damaged the owner's img");
    }

    // TWO LIVE PRESENTABLES (round-2 F10). Every arm above drives handle 0
    // alone, so the multi-slot row, the per-handle namespace and the I-32
    // sum across imgs had no runtime witness at all -- the ABI supports 16
    // and exactly one had ever been built.
    let second = format!("1 {} {} {} {} 0", W, H, FMT, STRIDE);
    if !write_ctl(root, &newp, &second) {
        img_fail("a SECOND presentable was refused while the first was live");
    }
    let info1 = open_read_string(root, &format!("ctx/{}/img/1/info", ctx));
    let res1 = parse_field(&info1, "res").unwrap_or_else(|| img_fail("second img info missing res"));
    if res1 == res {
        img_fail(&format!("two live presentables share res_id {}", res));
    }
    // The first must be UNDISTURBED by the second -- a slot row that aliased
    // would show up here and nowhere else.
    let info0b = open_read_string(root, &format!("ctx/{}/img/0/info", ctx));
    if parse_field(&info0b, "res") != Some(res) {
        img_fail("registering img 1 changed img 0's res_id");
    }
    if !write_ctl(root, &format!("ctx/{}/img/1/ctl", ctx), "destroy") {
        img_fail("second img ctl destroy refused");
    }
    if try_open_read(root, &format!("ctx/{}/img/0/info", ctx)).is_none() {
        img_fail("destroying img 1 also removed img 0");
    }

    // Destroy through the ctl verb, then prove BOTH halves of what destroy
    // means: the object is gone from the namespace, AND its handle is free
    // again. Checking only the first would pass on an implementation that
    // leaks the slot forever.
    if !write_ctl(root, &format!("ctx/{}/img/0/ctl", ctx), "destroy") {
        img_fail("img ctl destroy refused");
    }
    if try_open_read(root, &format!("ctx/{}/img/0/info", ctx)).is_some() {
        img_fail("img info still resolves AFTER destroy");
    }
    if !write_ctl(root, &newp, &good) {
        img_fail("the handle did not free on destroy (re-registration refused)");
    }
    let info2 = open_read_string(root, &format!("ctx/{}/img/0/info", ctx));
    let res2 = parse_field(&info2, "res").unwrap_or_else(|| img_fail("re-registered img info missing res"));
    if res2 == res {
        img_fail(&format!("re-registration reused res_id {} -- ids must be monotonic", res));
    }
    let _ = write_ctl(root, &format!("ctx/{}/img/0/ctl", ctx), "destroy");
    let _ = write_ctl(root, &format!("ctx/{}/ctl", ctx), "destroy");
    t_putstr(&format!(
        "warp-prove: IMG PASS (shape gate 4/4 refused; declared {}x{} stride {} fmt {} mem 0 echoed; xproc info+consent refused; two live imgs res {}/{} distinct + independent; res {} -> {} monotonic; handle freed on destroy)\n",
        W, H, STRIDE, FMT, res, res1, res, res2
    ));
    0
}

fn img_direct_fail(msg: &str) -> ! {
    t_putstr(&format!("warp-prove: IMG-DIRECT FAIL: {}\n", msg));
    unsafe { t_exits(1) }
}

/// W-3c-2: the presentable DIRECT arm, end-to-end over the wire -- the
/// generalized `present-to <surface> img <n>` consent, the F16 pending
/// switch completing on SET_SCANOUT_BLOB, and the display-safe
/// destroy-while-bound. The zoom chord that makes the surface fullscreen
/// comes from the expect harness (warp-img.exp), which also watches the
/// compositor's say lines; this side drives presents and observes `bound`
/// through `img/0/info` -- the guest-visible half of the same event.
fn img_direct_prove() -> i64 {
    use tapestry::Surface;
    let args = libthyla_rs::env::args();
    let dw: u32 = args.get_str(2).and_then(|s| s.parse().ok()).unwrap_or(1280);
    let dh: u32 = args.get_str(3).and_then(|s| s.parse().ok()).unwrap_or(800);
    t_putstr(&format!(
        "warp-prove: img-direct gate (W-3c-2, the presentable Direct arm) starting ({}x{})\n",
        dw, dh
    ));

    // The surface half of the mutual adoption, DISPLAY-sized: reconcile
    // takes a surface Direct only when the one visible leaf matches the
    // head, so the zoom can only complete on this shape.
    let mut surf = match Surface::open(dw, dh) {
        Ok(s) => s,
        Err(_) => img_direct_fail("tapestry surface open"),
    };
    for px in surf.pixels().iter_mut() {
        *px = 0xFF10_4020; // the weave content the restore leg falls back to
    }
    if surf.present(None).is_err() {
        img_direct_fail("first 2D present");
    }

    let warp = warp_connect("img-direct");
    let ctx = mint_ctx(warp, "img-direct");
    let stride = dw * 4;
    if !write_ctl(
        warp,
        &format!("ctx/{}/img/new", ctx),
        &format!("0 {} {} {} {} 0", dw, dh, VIRGL_FORMAT_B8G8R8A8_UNORM, stride),
    ) {
        t_putstr("warp-prove: IMG-DIRECT SKIP -- presentable mint refused (no venus device)\n");
        unsafe { t_exits(2) }
    }
    let info = open_read_string(warp, &format!("ctx/{}/img/0/info", ctx));
    let res = parse_field(&info, "res").unwrap_or_else(|| img_direct_fail("img info missing res"));

    if surf.surface_ctl(&format!("glsrc {}", ctx)).is_err() {
        img_direct_fail("glsrc");
    }
    if !write_ctl(warp, &format!("ctx/{}/ctl", ctx), &format!("present-to {} img 0", surf.id)) {
        img_direct_fail("present-to img refused");
    }
    t_putstr(&format!(
        "warp-prove: IMG-DIRECT armed (ctx {} res {} surf {})\n",
        ctx, res, surf.id
    ));

    // Present until the display binds the presentable. `bound` in info is
    // `bound_res == res` -- the guest-visible witness of the standing
    // SET_SCANOUT_BLOB binding (the spec's pbound). The zoom arrives from
    // the harness; F16 completes the switch only at a present-COMPLETE, so
    // keep presenting.
    let t0 = libthyla_rs::time::Instant::now();
    let mut bound = false;
    while (t0.elapsed().as_millis() as u64) < 120_000 {
        if surf.present(None).is_err() {
            img_direct_fail("present during the bind wait");
        }
        let now = open_read_string(warp, &format!("ctx/{}/img/0/info", ctx));
        if parse_field(&now, "bound") == Some(1) {
            bound = true;
            break;
        }
        let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(100));
    }
    if !bound {
        img_direct_fail("bound never reached 1 within 120s (zoom missing? head size mismatch?)");
    }
    t_putstr(&format!("warp-prove: IMG-DIRECT bound observed (res {})\n", res));

    // Destroy WHILE BOUND -- the display-safe teardown's first client-driven
    // execution: consent must clear server-side, the display must survive
    // the unbind, and the surface's own weave arm takes the scanout back at
    // a later present (the harness watches for `scanout direct N slot`).
    if !write_ctl(warp, &format!("ctx/{}/img/0/ctl", ctx), "destroy") {
        img_direct_fail("destroy-while-bound refused");
    }
    if try_open_read(warp, &format!("ctx/{}/img/0/info", ctx)).is_some() {
        img_direct_fail("img info still resolves after destroy");
    }
    for _ in 0..20 {
        if surf.present(None).is_err() {
            img_direct_fail("present during the restore");
        }
        let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(50));
    }
    // The destroy cleared the consent server-side; `off` must now be a
    // clean no-op, not an error.
    let _ = write_ctl(warp, &format!("ctx/{}/ctl", ctx), "present-to off");
    let _ = write_ctl(warp, &format!("ctx/{}/ctl", ctx), "destroy");
    t_putstr(&format!(
        "warp-prove: IMG-DIRECT PASS (armed -> bound -> destroyed-while-bound -> restored; ctx {} res {} {}x{})\n",
        ctx, res, dw, dh
    ));
    0
}

fn ring_host3d_prove() -> i64 {
    t_putstr("warp-prove: ring-host3d gate (V-3b-2) starting\n");
    let root = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv/warp".as_ptr(), 9, T_OREAD) };
    if root < 0 {
        ring_host3d_fail("open /srv/warp (is tapestryd serving?)");
    }
    // No virgl -> the ctx mint is unavailable (2D device) -> SKIP, like `ring`.
    let ctx = match try_open_read(root, "ctx/new").and_then(|s| parse_u32_prefix(&s)) {
        Some(v) => v,
        None => {
            t_putstr("warp-prove: RING-HOST3D SKIP -- no virgl on this device (ctx mint unavailable)\n");
            unsafe { t_exits(2) }
        }
    };

    // Mint the HOST3D ring: a venus-ctx-backed hostmem blob virglrenderer can
    // poll. Page-align the mint up from the Venus tight layout. A virgl-without-
    // venus device refuses the venus-ctx create (E_IO) -> the mint fails -> SKIP.
    let mint_bytes = (VN_RING_TOTAL + RING_PAGE - 1) & !(RING_PAGE - 1);
    if !write_ctl(root, &format!("ctx/{}/ring/new", ctx), &format!("{} 0 host3d", mint_bytes)) {
        t_putstr("warp-prove: RING-HOST3D SKIP -- host3d mint refused (no venus device)\n");
        unsafe { t_exits(2) }
    }

    // The res_id virglrenderer resolves vkCreateRingMESA.resourceId against.
    let info = open_read_string(root, &format!("ctx/{}/ring/0/info", ctx));
    let res_id = match parse_field(&info, "res") {
        Some(v) if v != 0 => v as u32,
        _ => ring_host3d_fail("host3d ring info missing/zero res_id"),
    };

    // Map the ring to read the status word. The ring is zeroed at install
    // (wring_install_host3d), and the host REQUIRES *head==0 && *status==0 at
    // create (vkr_ring.c:53); assert the zeroing holds before submitting.
    let map_fd = unsafe {
        let pth = format!("ctx/{}/ring/0/map", ctx);
        t_open(root, pth.as_ptr(), pth.len(), T_OREAD)
    };
    if map_fd < 0 {
        ring_host3d_fail("host3d ring/0/map open");
    }
    let va = unsafe { t_weft_map(map_fd as u64, 0) };
    if va < 0 {
        ring_host3d_fail("host3d ring t_weft_map claim");
    }
    let va = va as u64;
    if unsafe { rld32(va, VN_STATUS) } != 0 {
        ring_host3d_fail("host3d ring status != 0 at mint (host would reject the create)");
    }

    // Bootstrap: submit a bare vkCreateRingMESA. virglrenderer maps the same
    // res_id shmem, vkr_ring_create + vkr_ring_start it, and its poll thread sets
    // status&IDLE. idleTimeout=0 -> IDLE on the host's first poll iteration.
    let cmd = encode_vk_create_ring(res_id, VN_RING_TOTAL, 0);
    let submit_fd = unsafe {
        let p = format!("ctx/{}/submit", ctx);
        t_open(root, p.as_ptr(), p.len(), T_OWRITE)
    };
    if submit_fd < 0 {
        ring_host3d_fail("submit open");
    }
    let n = unsafe { t_write(submit_fd, cmd.b.as_ptr(), cmd.n) };
    unsafe { t_close(submit_fd) };
    if n < 0 {
        ring_host3d_fail("submit write (vkCreateRingMESA queue refused)");
    }

    // Witness: poll statusOffset for IDLE (0x1) set AND FATAL (0x2) clear. FATAL
    // is the host's decode/layout rejection of the hand-built bytes -- fail LOUD
    // + distinctly from a non-polling host. Bounded (~2 s): a host that never
    // maps+polls the ring FAILS, it does not hang.
    let mut idle = false;
    for _ in 0..200 {
        let st = unsafe { rld32(va, VN_STATUS) };
        if st & VK_RING_STATUS_FATAL != 0 {
            ring_host3d_fail("host set status&FATAL -- vkCreateRingMESA decode/layout rejected");
        }
        if st & VK_RING_STATUS_IDLE != 0 {
            idle = true;
            break;
        }
        let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(10));
    }
    if !idle {
        ring_host3d_fail("host never set status&IDLE -- virglrenderer did not map+poll the ring");
    }

    unsafe { t_close(map_fd) };
    unsafe { t_close(root) };
    t_putstr(
        "WARP-RING-HOST3D PASS (host3d mint + vkCreateRingMESA submit + host status&IDLE -- virglrenderer polls, no Mesa)\n",
    );
    0
}

fn ring_xproc_fail(msg: &str) -> ! {
    t_putstr("WARP-RING-XPROC FAIL: ");
    t_putstr(msg);
    t_putstr("\n");
    unsafe { t_exits(1) }
}

fn xproc_sleep_ms(ms: u64) {
    let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(ms));
}

/// Read the gpu-side host3d-ring reap ledger (park/reap) from the global warp ctl.
fn xproc_ledger(root: i64) -> (u64, u64) {
    // F4: a mid-poll ctl open failure must ride the WARP-RING-XPROC FAIL channel
    // (fast, scenario-named), not open_read_string's generic `fail`.
    let ctl = match try_open_read(root, "ctl") {
        Some(s) => s,
        None => ring_xproc_fail("ctl open refused mid-poll"),
    };
    let parked = parse_field(&ctl, "hostmem-ring-parked")
        .unwrap_or_else(|| ring_xproc_fail("ctl `hostmem-ring-parked` field missing"));
    let reaped = parse_field(&ctl, "hostmem-ring-reaped")
        .unwrap_or_else(|| ring_xproc_fail("ctl `hostmem-ring-reaped` field missing"));
    (parked, reaped)
}

/// Mint a fresh ctx + one HOST3D ring under it (ring index 0). Returns the ctx id,
/// or None if the device lacks virgl (no ctx) or venus (the host3d mint refuses)
/// -> the caller SKIPs. Minting a host3d ring runs `reap_hostmem_parked`
/// (reclaim-before-alloc), so this is also the lever that drives a reap pass.
fn xproc_mint_ring_ctx(root: i64) -> Option<u32> {
    let ctx = try_open_read(root, "ctx/new").and_then(|s| parse_u32_prefix(&s))?;
    let mint_bytes = (VN_RING_TOTAL + RING_PAGE - 1) & !(RING_PAGE - 1);
    if !write_ctl(root, &format!("ctx/{}/ring/new", ctx), &format!("{} 0 host3d", mint_bytes)) {
        return None;
    }
    Some(ctx)
}

/// V-3b-2 cross-Proc E2E: the host3d-ring park->reclaim LIFECYCLE (option d) plus
/// the cross-conn ring-ownership ISOLATION (option b). This is the only leg that
/// drives tapestryd's `retire_host3d_ring`/`reap_hostmem_parked` under a REAL
/// cross-Proc refcount (warp-prove's weft map is a genuine second-Proc ref on the
/// ring's hostmem burrow). Venus-gated: SKIPs on a 2D or virgl-without-venus
/// device, like `ring_host3d_prove`. The literal "cross-client-alias" race is
/// kernel-internal (t_weft_map claims+maps atomically) and is covered by the
/// white-box kernel test `weft.hostmem_refcount`, not here.
fn ring_xproc_prove() -> i64 {
    t_putstr("warp-prove: ring-xproc gate (V-3b-2 park->reclaim + isolation) starting\n");
    let root = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv/warp".as_ptr(), 9, T_OREAD) };
    if root < 0 {
        ring_xproc_fail("open /srv/warp (is tapestryd serving?)");
    }

    // === Phase 1: park -> reclaim lifecycle (option d) ===
    // Mint ctx A + ring R1 and MAP it: the map is a real cross-Proc ref that keeps
    // the ring's hostmem refcount > 1 across the retire below.
    let ctx_a = match xproc_mint_ring_ctx(root) {
        Some(c) => c,
        None => {
            t_putstr("warp-prove: RING-XPROC SKIP -- no venus device (host3d ctx/ring mint unavailable)\n");
            unsafe { t_exits(2) }
        }
    };
    let map_fd = unsafe {
        let p = format!("ctx/{}/ring/0/map", ctx_a);
        t_open(root, p.as_ptr(), p.len(), T_OREAD)
    };
    if map_fd < 0 {
        ring_xproc_fail("R1 ring/0/map open");
    }
    if unsafe { t_weft_map(map_fd as u64, 0) } < 0 {
        ring_xproc_fail("R1 t_weft_map claim");
    }
    let (park0, reap0) = xproc_ledger(root);

    // PARK: destroy ctx A while R1 is still mapped. tapestryd disarms the weft
    // share then retires R1; the live client map keeps refcount > 1, so it must
    // PARK (keep the offset), never free it under the client's PTEs.
    if !write_ctl(root, &format!("ctx/{}/ctl", ctx_a), "destroy") {
        ring_xproc_fail("ctx A destroy");
    }
    let mut parked = false;
    for _ in 0..200 {
        let (p, r) = xproc_ledger(root);
        if r != reap0 {
            ring_xproc_fail("ring RECLAIMED on a mapped retire -- reap fired under a live client map (I-45/I-7 alias)");
        }
        if p >= park0 + 1 {
            parked = true;
            break;
        }
        xproc_sleep_ms(10);
    }
    if !parked {
        ring_xproc_fail("ring did not PARK on a mapped retire (park count never advanced)");
    }

    // PARK-HELD: a fresh mint runs reap_hostmem_parked, but R1 is still mapped
    // (refcount > 1), so it must NOT be reclaimed -- the refcount GATES the reap.
    // tapestryd allows ONE ctx per conn (wctx_mint, counting retiring ones), so
    // capture this ctx: it must be destroyed before the reclaim mint below can
    // install its own on the same conn (audit F1).
    let ctx_b = match xproc_mint_ring_ctx(root) {
        Some(c) => c,
        None => ring_xproc_fail("park-held mint refused (no free ctx slot on this conn, or venus gone)"),
    };
    let (_, reap_mid) = xproc_ledger(root);
    if reap_mid != reap0 {
        ring_xproc_fail("parked ring RECLAIMED while still mapped -- the refcount did not gate the reap");
    }
    // Free the one-per-conn ctx slot. ctx B's ring is unmapped (refcount 1), so
    // its retire is an immediate drop -- neither ledger counter moves, and the
    // negative control just asserted stays valid.
    if !write_ctl(root, &format!("ctx/{}/ctl", ctx_b), "destroy") {
        ring_xproc_fail("park-held ctx B destroy");
    }

    // RELEASE: close R1's map fd. dev9p_close unmaps the client VA AND drops the
    // transferred registration pin (weft_binding_release), so the ring's total
    // refcount falls to 1 (tapestryd's own map alone).
    unsafe { t_close(map_fd) };

    // RECLAIM: the next mint's reap now finds R1 at refcount 1 -> reclaims it.
    // The conn's ctx slot is free again (ctx B destroyed). KEEP this ctx: phase 2
    // reuses it as the isolation target rather than minting a 4th on this conn.
    let ctx_c = match xproc_mint_ring_ctx(root) {
        Some(c) => c,
        None => ring_xproc_fail("reclaim mint refused (no free ctx slot on this conn, or venus gone)"),
    };
    let mut reaped = false;
    for _ in 0..200 {
        let (_, r) = xproc_ledger(root);
        if r >= reap0 + 1 {
            reaped = true;
            break;
        }
        xproc_sleep_ms(10);
    }
    if !reaped {
        ring_xproc_fail("parked ring not RECLAIMED after the client released it (reap count never advanced)");
    }

    // === Phase 2: cross-conn ring-ownership isolation (option b) ===
    // A hard tapestryd seam gate (wctx(id, conn) ownership), distinct from the
    // trusted-host res-scope the BO cross-ctx-blit probe reports. Pub-ids are
    // global + monotonic; a ctx is owned by its minting conn. ctx C is the still-
    // live RECLAIM ctx, reused (one ctx per conn).
    // Positive control (RESOURCE axis): the OWNER (conn A) reads its own ring
    // info, so a conn-B refusal below is ISOLATION, not the resource being absent.
    if try_open_read(root, &format!("ctx/{}/ring/0/info", ctx_c)).is_none() {
        ring_xproc_fail("isolation control: owner conn A could not read its OWN ring info");
    }
    let conn_b = warp_connect("ring-isolation B");
    // Positive control (CONN axis, audit F2): conn B can read the OWNERSHIP-FREE
    // global ctl, so a refusal below is the ownership GATE, not a wholesale-dead
    // second connection (aux#215: a negative satisfied by a broken fixture needs
    // a positive control one variable -- here the CONN -- away).
    let b_ctl = match try_open_read(conn_b, "ctl") {
        Some(s) => s,
        None => {
            unsafe { t_close(conn_b) };
            ring_xproc_fail("isolation control: conn B ctl open refused -- second conn is dead, not a gate test");
        }
    };
    if parse_field(&b_ctl, "hostmem-ring-parked").is_none() {
        unsafe { t_close(conn_b) };
        ring_xproc_fail("isolation control: conn B read an unparseable global ctl -- second conn is broken, not a gate test");
    }
    if try_open_read(conn_b, &format!("ctx/{}/ring/0/info", ctx_c)).is_some() {
        unsafe { t_close(conn_b) };
        ring_xproc_fail("ISOLATION BREACH: conn B READ conn A's ring info (ownership gate did not refuse)");
    }
    let b_submit = unsafe {
        let p = format!("ctx/{}/submit", ctx_c);
        t_open(conn_b, p.as_ptr(), p.len(), T_OWRITE)
    };
    if b_submit >= 0 {
        unsafe { t_close(b_submit) };
        unsafe { t_close(conn_b) };
        ring_xproc_fail("ISOLATION BREACH: conn B OPENED conn A's submit file (ownership gate did not refuse)");
    }
    unsafe { t_close(conn_b) };
    unsafe { t_close(root) };
    t_putstr(
        "WARP-RING-XPROC PASS (park-on-mapped-retire + park-held-under-refcount + reclaim-on-release + cross-conn ring isolation)\n",
    );
    0
}

fn warp_connect(what: &str) -> i64 {
    // The wait tells a full pool (refused at once) from a spent warp budget
    // (the listener unarmed: the kernel's handshake deadline).
    let t = libthyla_rs::time::Instant::now();
    let fd = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv/warp".as_ptr(), 9, T_OREAD) };
    if fd < 0 {
        fail(&format!("two-client: open /srv/warp for {} (rc {} after {} ms)", what, fd, t.elapsed().as_millis()));
    }
    fd
}

fn mint_ctx(root: i64, what: &str) -> u32 {
    match parse_u32_prefix(&open_read_string(root, "ctx/new")) {
        Some(v) => v,
        None => fail(&format!("two-client: ctx/new for {}", what)),
    }
}

/// A 1x1 render target -- the minimum the seam accepts, and enough to own a
/// real backing so `transfer_from` is a genuine fenced chain.
fn mint_backed_bo(root: i64, ctx: u32, what: &str) -> u32 {
    let bo = match parse_u32_prefix(&open_read_string(root, &format!("ctx/{}/bo/new", ctx))) {
        Some(v) => v,
        None => fail(&format!("two-client: bo/new for {}", what)),
    };
    let small = format!(
        "create3d {} {} {} 1 1 1 1 0 0 0 {}",
        PIPE_TEXTURE_2D, VIRGL_FORMAT_B8G8R8A8_UNORM, VIRGL_BIND_RENDER_TARGET, 4096
    );
    if !write_ctl(root, &format!("ctx/{}/bo/{}/ctl", ctx, bo), &small) {
        fail(&format!("two-client: create3d for {}", what));
    }
    bo
}

fn fenced_free(root: i64) -> u64 {
    parse_field(&open_read_string(root, "ctl"), "fenced-free")
        .unwrap_or_else(|| fail("ctl `fenced-free` field missing (test-mode build?)"))
}

/// AUDIT F9: the NON-FATAL ctx-ctl reader, for the C0 sections -- which are
/// documented to report a RESULT either way and to fail only on a broken
/// instrument. `ctx_field` below calls `fail()`, which prints the harness
/// HARD-FAIL token, and my own comment beside it claimed a pre-C-0d
/// tapestryd "would read 0 and never move". It would not: the field is
/// ABSENT, so the whole reject run aborted on the first read instead of
/// reaching the INSTRUMENT arm written for exactly that case.
///
/// Names the missing key, closes the scenario with its completion token so
/// the harness is not left waiting -- INCOMPLETE, since the C-0d Fable round
/// (F6) made DONE mean "every arm passed" -- and exits 0. The scenario
/// hard-fails on the token wherever it runs; the host gate additionally
/// fails on the ABSENT verdict terms.
fn ctx_field_soft(root: i64, ctx: u32, key: &str) -> u64 {
    match parse_field(&open_read_string(root, &format!("ctx/{}/ctl", ctx)), key) {
        Some(v) => v,
        None => {
            t_putstr(&format!(
                "warp-prove: C0-DETECT INSTRUMENT -- the ctx ctl carries no `{}` field. It is \
                 ABSENT, not 0 (a pre-C-0d tapestryd does not emit it at all), so nothing \
                 downstream could have meant anything.\n",
                key
            ));
            t_putstr(&format!("warp-prove: C0-REJECT INCOMPLETE(instrument:{})\n", key));
            unsafe { t_exits(0) }
        }
    }
}

/// ROUND-2 F8: the message names the KEY, not a leg. It used to be
/// hardcoded "two-client:", so under the harness truncation (which cuts a
/// line at the hard-fail token) a red run from any OTHER leg printed
/// `warp-prove: FAIL -- two-client: ctx ctl ` and sent the reader to the
/// wrong place entirely.
fn ctx_field(root: i64, ctx: u32, key: &str) -> u64 {
    parse_field(&open_read_string(root, &format!("ctx/{}/ctl", ctx)), key)
        .unwrap_or_else(|| fail(&format!("ctx {} ctl `{}` missing (test-mode build?)", ctx, key)))
}

/// Mint a BO of the prover's standard W x H render-target geometry and
/// return `(bo_id, res_id, mapped_va)`.
fn mint_sized_bo(root: i64, ctx: u32, what: &str) -> (u32, u32, u64) {
    let bo = match parse_u32_prefix(&open_read_string(root, &format!("ctx/{}/bo/new", ctx))) {
        Some(v) => v,
        None => fail(&format!("cross-blit: bo/new for {}", what)),
    };
    let create = format!(
        "create3d {} {} {} {} {} 1 1 0 0 0 {}",
        PIPE_TEXTURE_2D, VIRGL_FORMAT_B8G8R8A8_UNORM, VIRGL_BIND_RENDER_TARGET, W, H, BO_SIZE
    );
    if !write_ctl(root, &format!("ctx/{}/bo/{}/ctl", ctx, bo), &create) {
        fail(&format!("cross-blit: create3d for {}", what));
    }
    let res = parse_field(&open_read_string(root, &format!("ctx/{}/bo/{}/info", ctx, bo)), "res")
        .unwrap_or_else(|| fail(&format!("cross-blit: info `res` for {}", what))) as u32;
    let map_fd = unsafe {
        let p = format!("ctx/{}/bo/{}/map", ctx, bo);
        t_open(root, p.as_ptr(), p.len(), T_OREAD)
    };
    if map_fd < 0 {
        fail(&format!("cross-blit: map open for {}", what));
    }
    let va = unsafe { t_weft_map(map_fd as u64, 0) };
    if va < 0 {
        fail(&format!("cross-blit: weft_map for {}", what));
    }
    (bo, res, va as u64)
}

fn submit_stream(root: i64, ctx: u32, st: &[u32], what: &str) {
    let bytes: Vec<u8> = st.iter().flat_map(|d| d.to_le_bytes()).collect();
    let (n, refused) = write_behind_stall(root, &format!("ctx/{}/submit", ctx), &bytes);
    if n != bytes.len() as i64 {
        // n is the open's answer when the open failed, else the write's.
        fail(&format!("cross-blit: submit refused ({}, {}, after {} stall refusals)", what, n, refused));
    }
    note_stall_refusals(what, refused);
}

/// CREATE_SUB_CTX + SET_SUB_CTX. vrend scopes object state to a sub-context
/// and Mesa always makes one; emitted ONCE per context, since re-creating
/// sub-ctx 0 is not idempotent.
fn subctx_preamble(st: &mut Vec<u32>) {
    st.push(cmd0(VIRGL_CCMD_CREATE_SUB_CTX, 0, 1));
    st.push(0);
    st.push(cmd0(VIRGL_CCMD_SET_SUB_CTX, 0, 1));
    st.push(0);
}

/// A surface over `res_id` bound as cbuf0, then CLEAR to (r,g,b,1).
fn clear_stream(st: &mut Vec<u32>, res_id: u32, surf: u32, r: f32, g: f32, b: f32) {
    st.push(cmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, 5));
    st.push(surf);
    st.push(res_id);
    st.push(VIRGL_FORMAT_B8G8R8A8_UNORM);
    st.push(0); // level
    st.push(0); // layers
    st.push(cmd0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3));
    st.push(1); // nr_cbufs
    st.push(0); // zsurf
    st.push(surf);
    st.push(cmd0(VIRGL_CCMD_CLEAR, 0, 8));
    st.push(PIPE_CLEAR_COLOR0);
    st.push(r.to_bits());
    st.push(g.to_bits());
    st.push(b.to_bits());
    st.push(1.0f32.to_bits());
    st.push(0); // depth lo
    st.push(0); // depth hi
    st.push(0); // stencil
}

/// A full-surface BLIT, src -> dst. Field order is `virgl_protocol.h`
/// "blit" verbatim (21 payload dwords); the resource fields carry the raw
/// DEVICE-GLOBAL res id, which is exactly what `virgl_thylacine_emit_res`
/// writes for Mesa's own blits.
fn blit_stream(st: &mut Vec<u32>, src_res: u32, dst_res: u32) {
    st.push(cmd0(VIRGL_CCMD_BLIT, 0, VIRGL_CMD_BLIT_SIZE));
    st.push(PIPE_MASK_RGBA | (PIPE_TEX_FILTER_NEAREST << 8)); // S0; scissor off
    st.push(0); // scissor min x|y<<16
    st.push(0); // scissor max x|y<<16
    st.push(dst_res);
    st.push(0); // dst level
    st.push(VIRGL_FORMAT_B8G8R8A8_UNORM);
    st.push(0); // dst x
    st.push(0); // dst y
    st.push(0); // dst z
    st.push(W); // dst w
    st.push(H); // dst h
    st.push(1); // dst d
    st.push(src_res);
    st.push(0); // src level
    st.push(VIRGL_FORMAT_B8G8R8A8_UNORM);
    st.push(0); // src x
    st.push(0); // src y
    st.push(0); // src z
    st.push(W); // src w
    st.push(H); // src h
    st.push(1); // src d
}

/// Fail FAST if the last submit wedged the context.
///
/// Learned the hard way on the first Pi run: a mis-encoded BLIT made vrend
/// report `Illegal command buffer`, the context wedged, no fence ever
/// retired, and `resample`'s fence read -- which PARKS until a NEWER fence
/// arrives -- blocked forever. The harness then died on ITS timeout, so the
/// run reported "timeout waiting for WARP-PROVE PASS" instead of the actual
/// cause, which was sitting in the log two lines above. A bound on the
/// number of reads is not a bound on TIME when each read can park.
///
/// Best-effort by nature (the host reports the error asynchronously), but a
/// 9P round trip is usually enough for the poison to land, and being
/// occasionally too early only costs us the old behaviour.
fn guard_not_poisoned(root: i64, what: &str) {
    if let Some(v) = parse_field(&open_read_string(root, "ctl"), "poisoned") {
        if v != 0 {
            // Emit the reason on its OWN line first: the LS-CI harness
            // hard-fails on the token "warp-prove: FAIL" and kills the run
            // mid-line, so anything after that token never reaches the log.
            t_putstr("warp-prove: C0-P1 diagnosis follows\n");
            fail(&format!(
                "cross-blit: the seam reports poisoned={} after {} -- the submit was \
                 REJECTED (look for `Illegal command buffer` / `failed to dispatch` in \
                 the host log). Not a blit verdict: the stream never executed.",
                v, what
            ));
        }
    }
}

/// Re-sentinel the sample point, queue a readback, ride the fence stream
/// until the sentinel is overwritten, and return what landed.
///
/// Re-sentinelling before EVERY readback is what keeps SENTINEL meaning
/// exactly one thing -- "the readback never landed" -- on the second and
/// later calls, instead of decaying into "whatever the previous leg left".
fn resample(root: i64, ctx: u32, bo: u32, va: u64) -> u32 {
    let px0 = va as *mut u32;
    unsafe { core::ptr::write_volatile(px0, SENTINEL) };
    let before = ctx_field(root, ctx, "fence-signaled");
    let xfer = format!("transfer_from 0 0 0 0 {} {} 1 0 0 0", W, H);
    if !write_ctl(root, &format!("ctx/{}/bo/{}/ctl", ctx, bo), &xfer) {
        fail("cross-blit: transfer_from queue refused");
    }
    // Poll the ctx's MONOTONIC `fence-signaled` counter instead of parking
    // on the fence fd. Both halves of this were learned on the Pi:
    //
    //  - the fence FD PARKS until a NEWER fence retires, so a wedged ctx
    //    blocks forever. A bound on the number of READS is not a bound on
    //    TIME. The first run died on the harness's own 180 s timeout and
    //    reported "timeout waiting for WARP-PROVE PASS", burying the real
    //    cause (an illegal command buffer) two lines up the log.
    //  - the condition must be an INCREASE, not a level: a gauge reading
    //    zero is satisfied by "no fence was ever queued" exactly as well as
    //    by "the fence landed" (the seam's own ctl comment says so).
    //
    // Each poll is a 9P round trip, which is also what gives the host time
    // to report an async error into `poisoned`.
    for _ in 0..200 {
        if ctx_field(root, ctx, "poisoned") != 0 {
            return CTX_POISONED;
        }
        if ctx_field(root, ctx, "fence-signaled") > before {
            break;
        }
    }
    unsafe { core::ptr::read_volatile(px0) }
}

fn color_name(v: u32) -> &'static str {
    match v {
        SENTINEL => "SENTINEL(no readback)",
        CTX_POISONED => "CTX-POISONED(submit refused)",
        XFER_REFUSED => "XFER-REFUSED(transfer_from declined)",
        BLIT_RED => "RED(unmoved)",
        BLIT_GREEN => "GREEN(moved)",
        _ => "OTHER",
    }
}

/// Warp-C C-0 / P1 (`docs/GPU-DESIGN.md` §4.5.4): can a `VIRGL_CCMD_BLIT`
/// submitted on ONE context read a resource created by ANOTHER context?
///
/// Warp-C's whole design rests on this -- the compositor composes by
/// blitting each client's render target into a screen resource it owns, so
/// if a blit cannot cross the context boundary the design does not stand.
///
/// TWO questions, and the FIRST is about the seam we already ship:
///
///   P1a -- with NO attach of any kind, does a blit naming a foreign
///          device-global res id move pixels? A YES is a Warp-C green light
///          AND an **I-45 finding**: any warp client could read any other
///          client's framebuffer by naming its id, since `submit` takes an
///          opaque stream and the res fields are raw device-global ids.
///   P1b -- if P1a is NO, the seam needs an explicit cross-attach verb and
///          Warp-C's C-2 must add it. Either answer is a real result; only
///          a broken instrument is a failure.
///
/// THE INSTRUMENT'S OWN TRAP, and why this leg is shaped the way it is. A
/// blit that is REFUSED and a blit that is MIS-ENCODED produce identical
/// pixels, and in production both present as a BLACK SCREEN rather than an
/// error. So:
///
///   * the destination is cleared RED and the source GREEN, so "moved
///     nothing" and "moved the source" are different VALUES, not
///     presence-vs-absence;
///   * every readback re-sentinels first, so "the readback never landed" is
///     a THIRD distinguishable value and a broken instrument cannot
///     masquerade as a clean negative;
///   * a SAME-CONTEXT blit runs FIRST as the positive control. If that does
///     not turn the destination GREEN, the encoding here is wrong and NO
///     verdict about cross-context access is reportable. Without it, a
///     mis-encoded blit would read as "cross-context access is refused" --
///     the exact wrong-and-comforting answer, and the failure mode this
///     project keeps re-learning (a negative assertion satisfied by a
///     broken fixture).
fn prove_cross_ctx_blit() {
    let a = warp_connect("compositor A");
    let b = warp_connect("client B");
    let ctx_a = mint_ctx(a, "compositor A");
    let ctx_b = mint_ctx(b, "client B");
    if ctx_a == ctx_b {
        fail("cross-blit: A and B got the SAME ctx -- one connection, not two; \
              the cross-context question would be vacuous");
    }

    // A owns the destination ("the screen") and a local source for the
    // control; B owns the foreign source.
    let (dst_bo, dst_res, dst_va) = mint_sized_bo(a, ctx_a, "A dst/screen");
    let (_, local_res, _) = mint_sized_bo(a, ctx_a, "A local src (control)");
    let (_, foreign_res, _) = mint_sized_bo(b, ctx_b, "B src (foreign)");
    if local_res == foreign_res || dst_res == foreign_res {
        fail("cross-blit: resource ids collide -- the legs below would be vacuous");
    }

    // Paint: A's local source GREEN, B's foreign source GREEN, A's dst RED.
    // One sub-ctx preamble per context.
    let mut sa: Vec<u32> = Vec::new();
    subctx_preamble(&mut sa);
    clear_stream(&mut sa, local_res, 1, 0.0, 1.0, 0.0); // local src = GREEN
    clear_stream(&mut sa, dst_res, 2, 1.0, 0.0, 0.0); // dst        = RED
    submit_stream(a, ctx_a, &sa, "A paint");

    let mut sb: Vec<u32> = Vec::new();
    subctx_preamble(&mut sb);
    clear_stream(&mut sb, foreign_res, 1, 0.0, 1.0, 0.0); // foreign src = GREEN
    submit_stream(b, ctx_b, &sb, "B paint");

    // Baseline: the destination really is RED before any blit. Without this
    // a dst that was never painted would make the control's GREEN
    // unattributable.
    let base = resample(a, ctx_a, dst_bo, dst_va);
    if base != BLIT_RED {
        t_putstr(&format!(
            "warp-prove: C0-P1 INSTRUMENT: dst baseline is {} (0x{:08x}), want RED -- \
             the clear or the readback is broken. NO P1 VERDICT.\n",
            color_name(base), base
        ));
        return;
    }
    t_putstr("warp-prove: C0-P1 baseline dst=RED\n");

    // CONTROL: a SAME-context blit must move pixels. This validates the
    // BLIT encoding itself; a failure here invalidates the test below.
    let mut sc: Vec<u32> = Vec::new();
    blit_stream(&mut sc, local_res, dst_res);
    submit_stream(a, ctx_a, &sc, "control same-ctx blit");
    let ctl = resample(a, ctx_a, dst_bo, dst_va);
    if ctl != BLIT_GREEN {
        t_putstr(&format!(
            "warp-prove: C0-P1 INSTRUMENT: CONTROL same-ctx blit left {} (0x{:08x}), \
             want GREEN -- the BLIT encoding is wrong, so a cross-context refusal \
             below would really be an encoding bug. NO P1 VERDICT.\n",
            color_name(ctl), ctl
        ));
        return;
    }
    t_putstr("warp-prove: C0-P1 control same-ctx blit = GREEN (encoding valid)\n");

    // THE TEST: repaint dst RED, then blit from B's resource on A's context.
    let mut sr: Vec<u32> = Vec::new();
    clear_stream(&mut sr, dst_res, 3, 1.0, 0.0, 0.0);
    submit_stream(a, ctx_a, &sr, "repaint dst RED");
    let re = resample(a, ctx_a, dst_bo, dst_va);
    if re != BLIT_RED {
        t_putstr(&format!(
            "warp-prove: C0-P1 INSTRUMENT: dst repaint left {} (0x{:08x}), want RED -- \
             cannot attribute the cross-context result. NO P1 VERDICT.\n",
            color_name(re), re
        ));
        return;
    }

    let mut sx: Vec<u32> = Vec::new();
    blit_stream(&mut sx, foreign_res, dst_res);
    submit_stream(a, ctx_a, &sx, "cross-ctx blit");
    // NO guard_not_poisoned here, deliberately: a refused cross-context blit
    // POISONS the ctx, and that poison IS the measurement. `resample` reports
    // it as CTX_POISONED -> RESULT=REFUSED. Guarding here aborted the exact
    // outcome this leg exists to observe (run 3 on the Pi).
    let got = resample(a, ctx_a, dst_bo, dst_va);

    match got {
        BLIT_GREEN => {
            t_putstr(
                "warp-prove: C0-P1a RESULT=CROSSED -- a blit on ctx A read ctx B's \
                 resource with NO attach.\n",
            );
            t_putstr(
                "warp-prove: C0-P1a Warp-C is feasible AND this is an I-45 finding: \
                 a warp client can read any other client's framebuffer by naming its \
                 device-global res id.\n",
            );
        }
        BLIT_RED | CTX_POISONED => {
            t_putstr(
                "warp-prove: C0-P1a RESULT=REFUSED -- the cross-context blit did NOT \
                 read ctx B's resource (control passed, so the encoding is valid).\n",
            );
            t_putstr(
                "warp-prove: C0-P1a I-45 HOLDS on this host: vrend enforces the \
                 per-context resource bound even though `submit` carries an opaque \
                 stream of raw device-global ids.\n",
            );
            t_putstr(
                "warp-prove: C0-P1b Warp-C C-2 must add an explicit cross-attach verb; \
                 I-45's context bound holds here.\n",
            );
        }
        _ => {
            t_putstr(&format!(
                "warp-prove: C0-P1 INSTRUMENT: cross-ctx blit left {} (0x{:08x}) -- neither \
                 RED nor GREEN nor a poisoned ctx. NO P1 VERDICT.\n",
                color_name(got), got
            ));
        }
    }

    unsafe {
        t_close(a);
        t_close(b);
    }
}

/// #240: IS a vrend-rejected command stream observable from the guest, and
/// after how long?
///
/// The C-0 P1 run concluded "poisoned stays 0, the fence never signals, so a
/// refusal is indistinguishable from a hang". That conclusion came out of a
/// 200-ITERATION poll -- a few hundred ms of 9P round trips -- while the only
/// mechanism that can report a never-retiring fence is tapestryd's reaper at
/// FENCE_ABANDON_MS = 30 s. So the finding described this PROBE'S BUDGET and
/// was recorded as a property of the seam. Give the reaper the time it needs
/// and report what actually arrives.
///
/// The healthy ctx is sampled in the SAME window on purpose: "the bad ctx
/// poisons at 30 s" cannot be told apart from "every ctx poisons at 30 s" by
/// watching the bad one alone, and an indiscriminate reaper would read
/// identically. It carries real fenced work for the same reason -- a ctx with
/// nothing in flight is never a reap candidate, so its clean reading would be
/// satisfied by a completely broken reaper.
///
/// Report-only: this leg MEASURES an open question, so it must never emit a
/// verdict token. A probe's output is data.
fn observe_rejection() {
    const OBSERVE_MS: u64 = 45_000;
    const SAMPLE_MS: u64 = 250;

    t_putstr("warp-prove: C0-REJECT observing a rejected stream for 45 s\n");

    let bad = warp_connect("reject/bad");
    let ok = warp_connect("reject/ok");
    let ctx_bad = mint_ctx(bad, "reject/bad");
    let ctx_ok = mint_ctx(ok, "reject/ok");

    // Both ctxs are built identically, so the ONLY difference between them
    // is whether vrend accepts the stream. The first cut of this leg gave
    // the control a `transfer_from` instead of a submit -- a different
    // CLASS of fenced work, which could not have distinguished "the
    // rejected submit's fence retired" from "the count I read was the ctx
    // build's, and the submit's fence was simply lost" (#212).
    let (bad_bo, bad_res, bad_va) = mint_sized_bo(bad, ctx_bad, "reject/bad");
    let (ok_bo, ok_res, ok_va) = mint_sized_bo(ok, ctx_ok, "reject/ok");

    // Read the counters BEFORE either submit: ctx create and the backing
    // build are fenced work too, so a LEVEL reading would credit them to
    // the stream. Only the delta across the submit is attributable to it.
    let bad_before = ctx_field(bad, ctx_bad, "fence-signaled");
    let ok_before = ctx_field(ok, ctx_ok, "fence-signaled");
    t_putstr(&format!(
        "warp-prove: C0-REJECT pre-submit fence-signaled: bad {} ok {}\n",
        bad_before, ok_before
    ));

    // The CONTROL: a VALID stream through the same submit path.
    let mut sok: Vec<u32> = Vec::new();
    subctx_preamble(&mut sok);
    clear_stream(&mut sok, ok_res, 1, 1.0, 0.0, 0.0);
    submit_stream(ok, ctx_ok, &sok, "reject/ok valid stream");
    let _ = bad_res;

    // The rejection. A CLEAR with a zero-dword payload is what the log
    // already shows vrend refusing ("Illegal command buffer 7"), and it
    // needs no resource of its own, so nothing but the stream is on trial.
    let submit_path = format!("ctx/{}/submit", ctx_bad);
    let fd = unsafe { t_open(bad, submit_path.as_ptr(), submit_path.len(), T_OWRITE) };
    if fd < 0 {
        t_putstr("warp-prove: C0-REJECT submit open failed\n");
        unsafe {
            t_close(bad);
            t_close(ok);
        }
        return;
    }
    let nop = cmd0(VIRGL_CCMD_CLEAR, 0, 0).to_le_bytes();
    let wrote = unsafe { t_write(fd, nop.as_ptr(), nop.len()) };
    unsafe { t_close(fd) };
    t_putstr(&format!(
        "warp-prove: C0-REJECT submitted a malformed stream (write rc={})\n",
        wrote
    ));

    let t0 = libthyla_rs::time::Instant::now();
    let mut prev: Option<(u64, u64, u64, u64, u64, u64)> = None;
    let mut poison_ms: Option<u64> = None;
    let mut bad_retired_ms: Option<u64> = None;
    let mut ok_retired_ms: Option<u64> = None;
    loop {
        let el = t0.elapsed().as_millis() as u64;
        if el > OBSERVE_MS {
            break;
        }
        let s = (
            ctx_field(bad, ctx_bad, "poisoned"),
            ctx_field(bad, ctx_bad, "fence-signaled"),
            ctx_field(bad, ctx_bad, "fences-in-flight"),
            ctx_field(ok, ctx_ok, "poisoned"),
            ctx_field(ok, ctx_ok, "fence-signaled"),
            ctx_field(ok, ctx_ok, "fences-in-flight"),
        );
        if prev != Some(s) {
            t_putstr(&format!(
                "warp-prove: C0-REJECT t={:>6}ms bad(poison {} sig {} inflight {}) \
                 ok(poison {} sig {} inflight {})\n",
                el, s.0, s.1, s.2, s.3, s.4, s.5
            ));
            prev = Some(s);
        }
        if s.0 != 0 && poison_ms.is_none() {
            poison_ms = Some(el);
        }
        if s.1 > bad_before && bad_retired_ms.is_none() {
            bad_retired_ms = Some(el);
        }
        if s.4 > ok_before && ok_retired_ms.is_none() {
            ok_retired_ms = Some(el);
        }
        // Once BOTH submits have resolved one way or the other there is
        // nothing left for the 30 s reaper to change; stop early only when
        // the rejected one has genuinely retired, since "still in flight"
        // is exactly the state the long window exists to watch.
        if bad_retired_ms.is_some() && ok_retired_ms.is_some() && el > 2 * SAMPLE_MS {
            break;
        }
        if let Some(p) = poison_ms {
            if el > p + 2 * SAMPLE_MS {
                break;
            }
        }
        let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(SAMPLE_MS));
    }

    let _ = match (bad_retired_ms, ok_retired_ms, poison_ms) {
        // The control never moved: nothing below is interpretable.
        (_, None, _) => t_putstr(
            "warp-prove: C0-REJECT INSTRUMENT -- the VALID control stream never retired \
             its fence, so this run cannot say anything about the rejected one.\n",
        ),
        (Some(b), Some(o), None) => t_putstr(&format!(
            "warp-prove: C0-REJECT ANSWER=REPORTED-AS-SUCCESS -- the REJECTED stream \
             retired its fence at {}ms (control {}ms) and never poisoned. Every \
             guest-visible channel says the work completed. A refusal is \
             indistinguishable from SUCCESS, not from a hang.\n",
            b, o
        )),
        (None, Some(o), Some(p)) => t_putstr(&format!(
            "warp-prove: C0-REJECT ANSWER=OBSERVABLE-AT-{}ms -- the rejected stream never \
             retired and the ctx poisoned, while the control retired at {}ms. The signal \
             is a TIMEOUT: it reports `never completed`, NOT `refused`.\n",
            p, o
        )),
        (None, Some(o), None) => t_putstr(&format!(
            "warp-prove: C0-REJECT ANSWER=SILENTLY-LOST -- the rejected stream neither \
             retired its fence nor poisoned within {} ms, while the control retired at \
             {}ms. The client waits forever with no signal.\n",
            OBSERVE_MS, o
        )),
        (Some(b), Some(o), Some(p)) => t_putstr(&format!(
            "warp-prove: C0-REJECT ANSWER=RETIRED-THEN-POISONED -- rejected retired at \
             {}ms (control {}ms) but the ctx ALSO poisoned at {}ms. Contradictory; read \
             the timeline above before drawing anything.\n",
            b, o, p
        )),
    };
    // STICKINESS. vrend latches a context error, so the question a
    // compositor actually cares about is not "was the bad stream lost" but
    // "is the ctx dead from here on -- while still reporting success?".
    // Submit a VALID stream on BOTH ctxs and read the pixels back: the
    // fence counters cannot answer this, since (per the timeline above)
    // they say `completed` either way. Pixels are the only honest channel.
    let mut sfix: Vec<u32> = Vec::new();
    subctx_preamble(&mut sfix);
    clear_stream(&mut sfix, bad_res, 1, 0.0, 1.0, 0.0); // GREEN
    submit_stream(bad, ctx_bad, &sfix, "reject/bad recovery stream");
    let mut sfix2: Vec<u32> = Vec::new();
    subctx_preamble(&mut sfix2);
    clear_stream(&mut sfix2, ok_res, 1, 0.0, 1.0, 0.0); // GREEN
    submit_stream(ok, ctx_ok, &sfix2, "reject/ok second stream");

    let after_bad = quiet_readback(bad, ctx_bad, bad_bo, bad_va);
    let after_ok = quiet_readback(ok, ctx_ok, ok_bo, ok_va);
    t_putstr(&format!(
        "warp-prove: C0-REJECT recovery: bad reads {} (0x{:08x}), ok reads {} (0x{:08x})\n",
        color_name(after_bad), after_bad, color_name(after_ok), after_ok
    ));
    let _ = match (after_bad == BLIT_GREEN, after_ok == BLIT_GREEN) {
        (_, false) => t_putstr(
            "warp-prove: C0-REJECT STICKY=NO-CONTROL -- the healthy ctx's own valid \
             stream did not land either, so the bad ctx's reading proves nothing.\n",
        ),
        (false, true) => t_putstr(
            "warp-prove: C0-REJECT STICKY=YES -- a VALID stream on the rejected ctx \
             still moves no pixels while the same stream works on a fresh ctx. One bad \
             submit kills the context PERMANENTLY, and every fence still says success.\n",
        ),
        (true, true) => t_putstr(
            "warp-prove: C0-REJECT STICKY=NO -- the rejected ctx accepts later valid \
             work. The damage is confined to the refused stream.\n",
        ),
    };
    // THE DETECTOR (#240 fix, GPU-DESIGN 4.5.4b). Everything above measures
    // the raw channels and shows they cannot tell refusal from success; this
    // asks the new one.
    //
    // BOTH DIRECTIONS ARE THE GATE. A detector that latches on everything
    // passes "the rejected ctx reports 1" on its own, and this arc has
    // already shipped two assertions that were satisfied by a broken fixture
    // (#212, aux#215). The healthy ctx is the same class of client running
    // the same verb, so the ONLY difference between the two readings is
    // whether the host refused a stream.
    //
    // `verify-seq` is read first and required to MOVE: `stream-rejected 0`
    // is equally satisfied by "the probe ran and found health" and by "the
    // probe never ran at all" (#184, the gauge-reading-zero trap).
    let vs_bad_0 = ctx_field_soft(bad, ctx_bad, "verify-seq");
    let vs_ok_0 = ctx_field_soft(ok, ctx_ok, "verify-seq");
    let wrote_bad = write_ctl(bad, &format!("ctx/{}/ctl", ctx_bad), "verify");
    let wrote_ok = write_ctl(ok, &format!("ctx/{}/ctl", ctx_ok), "verify");
    let vs_bad = ctx_field_soft(bad, ctx_bad, "verify-seq");
    let vs_ok = ctx_field_soft(ok, ctx_ok, "verify-seq");
    let sr_bad = ctx_field_soft(bad, ctx_bad, "stream-rejected");
    let sr_ok = ctx_field_soft(ok, ctx_ok, "stream-rejected");
    let at_bad = ctx_field_soft(bad, ctx_bad, "rejected-at");
    // AUDIT F2: the healthy arm's `stream-rejected 0` is ALSO satisfied by a
    // probe that returned UNKNOWN, so on its own it is a negative assertion a
    // broken fixture passes (the aux#215 class, which this arc has now
    // shipped three times). `verify-ok` advances ONLY on a healthy verdict,
    // so requiring it to move is what makes the control mean "asked and
    // found healthy" rather than merely "not reported rejected".
    let vok_ok = ctx_field_soft(ok, ctx_ok, "verify-ok");
    t_putstr(&format!(
        "warp-prove: C0-DETECT verify wrote(bad {} ok {}) verify-seq(bad {}->{} ok {}->{}) \
         stream-rejected(bad {} ok {}) rejected-at(bad {})\n",
        wrote_bad as u32, wrote_ok as u32, vs_bad_0, vs_bad, vs_ok_0, vs_ok, sr_bad, sr_ok, at_bad
    ));
    // Every C0 arm records whether it PASSED (C-0d Fable round F6): the
    // scenario's completion token used to print unconditionally, so a
    // FAIL(vacuous) / FAIL(blind) arm reached the prompt with the same
    // `C0-REJECT DONE` a green run prints, and only the host-side 5-term
    // grep stood between a blind detector and a pass. Now DONE means every
    // arm passed; anything else prints INCOMPLETE(<arm>) and the scenario
    // hard-fails on it wherever it runs.
    let detect_pass = if vs_bad <= vs_bad_0 || vs_ok <= vs_ok_0 {
        t_putstr(
            "warp-prove: C0-DETECT INSTRUMENT -- `verify-seq` did not advance on one or both \
             ctxs, so the probe did not run and neither reading below means anything. \
             (A pre-C-0d tapestryd cannot reach here at all: the field is ABSENT and \
             `ctx_field_soft` reports that separately.)\n",
        );
        false
    } else if sr_bad == 1 && sr_ok == 0 && vok_ok == 0 {
        t_putstr(
            "warp-prove: C0-DETECT FAIL(control unproven) -- the healthy ctx reports \
             stream-rejected 0 but `verify-ok` never advanced, so its probe returned \
             UNKNOWN rather than finding health. The control is vacuous (F2).\n",
        );
        false
    } else if sr_bad == 1 && sr_ok == 0 {
        t_putstr(&format!(
            "warp-prove: C0-DETECT PASS -- the REJECTED ctx reports stream-rejected 1 \
             (at verify {}) while the healthy ctx running the SAME verb reports 0 AND \
             recorded a healthy verdict (verify-ok {}). The detector discriminates; \
             #240 is observable in-guest.\n",
            at_bad, vok_ok
        ));
        true
    } else if sr_bad == 1 && sr_ok == 1 {
        t_putstr(
            "warp-prove: C0-DETECT FAIL(vacuous) -- BOTH ctxs report stream-rejected 1. \
             The detector latches on health too, so its positive reading proves nothing.\n",
        );
        false
    } else if sr_bad == 0 && sr_ok == 0 {
        t_putstr(
            "warp-prove: C0-DETECT FAIL(blind) -- the rejected ctx reports 0 after a verify \
             that DID run. The probe's copy reached the host on a ctx vrend had latched, \
             or the seed/readback is not landing where the compare reads.\n",
        );
        false
    } else {
        t_putstr(
            "warp-prove: C0-DETECT FAIL(inverted) -- the HEALTHY ctx reports rejected and the \
             refused one does not. The two arms are crossed.\n",
        );
        false
    };
    // Sticky is the contract (recreate, never retry): a second verify on the
    // healthy ctx must not drift, and on the rejected one must not clear.
    //
    // THE SLEEP IS LOAD-BEARING, and its absence made this leg vacuous. The
    // server rate-limits to one probe per ctx per compositor tick, so a
    // re-verify issued microseconds after the first is answered from the
    // state the first one established -- it would agree with the first
    // reading no matter what a second real probe would have found. Sleep
    // past a tick (60 Hz -> ~17 ms; 100 ms is comfortable), then REQUIRE
    // `verify-seq` to have advanced before believing the re-read, and say so
    // plainly when it has not.
    let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(100));
    //
    // AUDIT F10: half of this leg used to be a TAUTOLOGY. Nothing in the
    // server ever sets `stream_rejected` back to false -- full writer census
    // -- so "the rejection did not clear" was unfalsifiable, and the ctl
    // field cannot tell "the second probe also found rejection" from "the
    // flag is simply still set". `verify-ok` is the falsifiable twin: it
    // advances ONLY when a probe reaches a HEALTHY verdict, so a second real
    // probe must leave it STILL on the rejected ctx and MOVE it on the
    // control. `rejected-at` rides along as the re-latch check -- it is
    // written once, at first detection, and must stay pinned there.
    let rv_bad_0 = ctx_field_soft(bad, ctx_bad, "verify-seq");
    let rv_ok_0 = ctx_field_soft(ok, ctx_ok, "verify-seq");
    let vok_bad_0 = ctx_field_soft(bad, ctx_bad, "verify-ok");
    let vok_ok_0 = ctx_field_soft(ok, ctx_ok, "verify-ok");
    let at_bad_0 = ctx_field_soft(bad, ctx_bad, "rejected-at");
    let _ = write_ctl(bad, &format!("ctx/{}/ctl", ctx_bad), "verify");
    let _ = write_ctl(ok, &format!("ctx/{}/ctl", ctx_ok), "verify");
    let rv_bad = ctx_field_soft(bad, ctx_bad, "verify-seq");
    let rv_ok = ctx_field_soft(ok, ctx_ok, "verify-seq");
    let sr2_bad = ctx_field_soft(bad, ctx_bad, "stream-rejected");
    let sr2_ok = ctx_field_soft(ok, ctx_ok, "stream-rejected");
    let vok_bad = ctx_field_soft(bad, ctx_bad, "verify-ok");
    let vok_ok = ctx_field_soft(ok, ctx_ok, "verify-ok");
    let at_bad = ctx_field_soft(bad, ctx_bad, "rejected-at");
    let sticky_pass = if rv_bad <= rv_bad_0 || rv_ok <= rv_ok_0 {
        t_putstr(&format!(
            "warp-prove: C0-DETECT STICKY NOT TESTED -- the re-verify was rate-limited \
             (verify-seq bad {}->{} ok {}->{}), so the second reading is the first one's \
             cache and proves nothing about stickiness.\n",
            rv_bad_0, rv_bad, rv_ok_0, rv_ok
        ));
        false
    } else if sr2_bad == 1 && sr2_ok == 0 && vok_bad == vok_bad_0 && vok_ok > vok_ok_0 {
        t_putstr(&format!(
            "warp-prove: C0-DETECT STICKY PASS -- a SECOND real probe (verify-seq bad {} ok \
             {}) reads (1 0), and the falsifiable half holds: verify-ok stayed {} on the \
             rejected ctx (it found no health) while the control's moved {}->{}. \
             rejected-at pinned at {}.\n",
            rv_bad, rv_ok, vok_bad, vok_ok_0, vok_ok, at_bad
        ));
        true
    } else if at_bad != at_bad_0 {
        t_putstr(&format!(
            "warp-prove: C0-DETECT STICKY FAIL(re-latched) -- `rejected-at` moved {}->{}, so \
             the ctx was re-detected rather than staying latched at first detection.\n",
            at_bad_0, at_bad
        ));
        false
    } else if vok_bad > vok_bad_0 {
        t_putstr(&format!(
            "warp-prove: C0-DETECT STICKY FAIL(healed) -- a second real probe found the \
             REJECTED ctx healthy (verify-ok {}->{}) while stream-rejected still reads {}. \
             The flag is sticky but the probe is not: the two now disagree.\n",
            vok_bad_0, vok_bad, sr2_bad
        ));
        false
    } else {
        t_putstr(&format!(
            "warp-prove: C0-DETECT STICKY FAIL -- a second real probe reads (bad {} ok {}), \
             want (1 0); verify-ok bad {}->{} ok {}->{} (want still / moved).\n",
            sr2_bad, sr2_ok, vok_bad_0, vok_bad, vok_ok_0, vok_ok
        ));
        false
    };

    // ===== F1: CAN A CLIENT BLIND ITS OWN DETECTOR? =====
    //
    // The C-0d audit's headline finding, and it is filed as proof-by-
    // scripture: the reviewer could not read virglrenderer in-tree, so the
    // central step -- that a client can name a resource the SERVER attached
    // to the client's OWN dev_ctx -- rests on in-repo statements plus P1a's
    // same-context control. Convert it to proof-by-measurement before any
    // fix is designed around it.
    //
    // THE ATTACK, exactly as filed. `res_seq` is one pre-incremented counter
    // shared by probes and client BOs, and a ctx mint consumes exactly two
    // ids for its probe -- so with the client's first BO reporting `res` N,
    // the probe's are N-2 (mark) and N-1 (sentinel). The client then writes
    // MARK, which is painted once at ctx create and never repainted. Every
    // later verify should read neither mark nor token -> UNKNOWN -> and with
    // F2 (UNKNOWN has no ctl representation) that reads as HEALTHY forever.
    //
    // Uses a THIRD connection so neither arm above is disturbed.
    //
    // THE SOURCE IS A BUFFER (C-0d Fable round F1). The probe pair under
    // attack is minted as BUFFER resources now (`warp_hprobe_build`; the
    // texture pair only where that mint fails), and a texture->buffer
    // `RESOURCE_COPY_REGION` is not a legal copy: with a texture source the
    // renderer would drop the copy on its own and this leg would read
    // DEFENDED for the wrong reason -- a control the operation erases. So
    // the attacker mints a buffer of the probe's own shape, fills its first
    // 4 bytes with its own value, pushes them to the host, and copies 4
    // BYTES over the guessed mark: the same command the server's own verify
    // issues, exactly as the finding filed it.
    let f1 = warp_connect("f1/blind");
    let ctx_f1 = mint_ctx(f1, "f1/blind");
    let (f1_bo, f1_res, f1_va) = mint_buffer_bo(f1, ctx_f1, "f1/blind");
    // Derive the probe ids from OUR OWN first resource id, exactly as an
    // attacker would -- do not hardcode, or the leg stops tracking the
    // server's allocation order and silently tests nothing.
    let guess_mark = f1_res.wrapping_sub(2);
    let guess_sent = f1_res.wrapping_sub(1);
    t_putstr(&format!(
        "warp-prove: C0-F1 our first res {} (a buffer) -> guessing mark {} sentinel {}\n",
        f1_res, guess_mark, guess_sent
    ));
    // Baseline: the ctx must be HEALTHY and the probe must WORK before the
    // attack, or a post-attack blindness reading is unattributable.
    let _ = write_ctl(f1, &format!("ctx/{}/ctl", ctx_f1), "verify");
    let f1_sr0 = ctx_field_soft(f1, ctx_f1, "stream-rejected");
    let f1_vs0 = ctx_field_soft(f1, ctx_f1, "verify-seq");
    let f1_ok0 = ctx_field_soft(f1, ctx_f1, "verify-ok");
    // Fill our own buffer with the client's green, push it to the host
    // (the fenced transfer verb; it decodes ahead of the submit that
    // follows, one in-order controlq), then copy IT over the guessed mark.
    unsafe { core::ptr::write_volatile(f1_va as *mut u32, 0xFF00_FF00) };
    if !write_ctl(
        f1,
        &format!("ctx/{}/bo/{}/ctl", ctx_f1, f1_bo),
        "transfer_to 0 0 0 0 4 1 1 0 0 0",
    ) {
        fail("f1: transfer_to of the attacker's 4 bytes");
    }
    let mut sc2: Vec<u32> = Vec::new();
    subctx_preamble(&mut sc2);
    rcr_stream(&mut sc2, f1_res, guess_mark, 4);
    submit_stream(f1, ctx_f1, &sc2, "f1 overwrite the probe MARK");
    // THE POSITIVE CONTROL (added when the source became a buffer): DEFENDED
    // below is "verify-ok still advanced", which an attack that never landed
    // satisfies just as well (aux#215 -- a negative assertion is satisfied
    // by a broken fixture). The texture-era leg leaned on a one-time
    // host-log measurement for that; the buffer form re-earns it IN-GUEST:
    // copy the mark BACK into our own buffer (the same command the other
    // way), read our buffer back, and require the client's green -- so the
    // leg proves a client can WRITE the probe's mark AND READ it, exactly as
    // the finding filed it, before it claims the repaint held. The
    // readback rides the same in-order controlq behind both copies. If the
    // mark reads PROBE_MARK the attack did not land and nothing after it is
    // attributable; a stale SENTINEL means the readback itself never landed.
    unsafe { core::ptr::write_volatile(f1_va as *mut u32, SENTINEL) };
    let mut sc3: Vec<u32> = Vec::new();
    rcr_stream(&mut sc3, guess_mark, f1_res, 4);
    submit_stream(f1, ctx_f1, &sc3, "f1 read the probe MARK back into our buffer");
    let f1_sig0 = ctx_field_soft(f1, ctx_f1, "fence-signaled");
    if !write_ctl(
        f1,
        &format!("ctx/{}/bo/{}/ctl", ctx_f1, f1_bo),
        "transfer_from 0 0 0 0 4 1 1 0 0 0",
    ) {
        fail("f1: transfer_from of our own buffer");
    }
    for _ in 0..200 {
        if ctx_field_soft(f1, ctx_f1, "fence-signaled") > f1_sig0 {
            break;
        }
    }
    let f1_mark_seen = unsafe { core::ptr::read_volatile(f1_va as *const u32) };
    let f1_landed = f1_mark_seen == 0xFF00_FF00;
    if f1_landed {
        t_putstr(&format!(
            "warp-prove: C0-F1 ATTACK LANDED -- the mark read back through our own buffer as \
             {:#010x}: a client can both WRITE and READ the probe's resources (the finding, \
             re-measured in-guest on the buffer pair)\n",
            f1_mark_seen
        ));
    } else {
        t_putstr(&format!(
            "warp-prove: C0-F1 INSTRUMENT -- the mark read back as {:#010x} through our own \
             buffer (want the client's green {:#010x}; PROBE_MARK = the copy did not land, \
             SENTINEL {:#010x} = the readback did not land), so a DEFENDED reading below would \
             be vacuous.\n",
            f1_mark_seen, 0xFF00_FF00u32, SENTINEL
        ));
    }
    let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(100));
    let _ = write_ctl(f1, &format!("ctx/{}/ctl", ctx_f1), "verify");
    let f1_sr = ctx_field_soft(f1, ctx_f1, "stream-rejected");
    let f1_vs = ctx_field_soft(f1, ctx_f1, "verify-seq");
    let f1_ok = ctx_field_soft(f1, ctx_f1, "verify-ok");
    t_putstr(&format!(
        "warp-prove: C0-F1 before(sr {} vs {} ok {}) after(sr {} vs {} ok {})\n",
        f1_sr0, f1_vs0, f1_ok0, f1_sr, f1_vs, f1_ok
    ));
    // ROUND-2 F8: `verify-seq` advancing plus `stream-rejected 0` is EXACTLY
    // the reading an UNKNOWN produces -- the round's own F2 lesson, not
    // applied here when it was learned. `verify-ok` is the only predicate
    // that means "asked and found healthy", so the baseline requires it.
    let f1_defended = if f1_vs0 == 0 || f1_sr0 != 0 || f1_ok0 == 0 {
        t_putstr(
            "warp-prove: C0-F1 INSTRUMENT -- the ctx was not verifiably healthy BEFORE the \
             attack (needs verify-seq moved AND verify-ok moved AND stream-rejected 0), so \
             nothing after it is attributable.\n",
        );
        false
    } else if !f1_landed {
        // Reported above; an unlanded attack proves nothing about the defence.
        false
    } else if f1_vs <= f1_vs0 {
        t_putstr(
            "warp-prove: C0-F1 INSTRUMENT -- the post-attack verify did not run (rate limit?), \
             so this says nothing.\n",
        );
        false
    } else {
        // Decided IN-GUEST off `verify-ok`, which advances ONLY on a healthy
        // verdict. Blinding shows up as verify-seq advancing while verify-ok
        // does not -- the UNKNOWN signature -- with no host-log reading
        // needed. The first cut of this verdict told the reader to grep for a
        // phrase it PRINTED ITSELF, so its own output matched the search and
        // the check could never report absence (#186, in my own instrument).
        if f1_ok > f1_ok0 {
            t_putstr(
                "warp-prove: C0-F1 DEFENDED -- after a client wrote the probe's mark, the \
                 next verify still reached a HEALTHY verdict (verify-ok advanced). The \
                 per-verify repaint holds: corruption cannot outlive one verify.\n",
            );
            true
        } else {
            t_putstr(
                "warp-prove: C0-F1 BLINDED -- verify-seq advanced but verify-ok did NOT, so \
                 the probe returned UNKNOWN after the client wrote its mark. The detector is \
                 blindable for the ctx's life and a dead ctx would read as healthy.\n",
            );
            false
        }
    };

    // FOLLOW-UP ROUND F1 [P1] regression, and it is the INVERSE of what this
    // leg asserted when it was written. The C-6b close added a create-time
    // lower bound on a B8G8R8A8 backing and this leg proved it refused
    // `512 512 ... 4096`. The follow-up round found that shape is what THIS
    // PROJECT'S OWN Mesa winsys emits for a legitimate texture --
    // `usr/ports/mesa/patches/0006-*.patch:1511`, at the line that picks the
    // size: "the driver's staging-path textures legitimately ask for size 1".
    // Mesa declares one byte on two paths that keep the real width/height
    // (the staging path, and MSAA which asks for no guest backing at all),
    // the winsys rounds it to one page, and the result is byte-for-byte the
    // "attack" this leg demanded be refused. There was nothing to tell apart.
    //
    // So the door must ADMIT it, and this leg now proves that -- the
    // regression it guards is a compositor that refuses ordinary GL
    // allocations. The MSAA arm needed no host capability, so before the
    // removal every multisampled BGRA target above 32x32 was refused outright.
    //
    // The CONTROL is the other direction, one variable away: a genuinely
    // malformed declaration (unaligned backing) must still be REFUSED, so
    // "admitted" cannot pass against a create3d that admits everything.
    //
    // The P0's real guard is the READ gate in `gl_adoption`, which is exact,
    // re-checked at retire, and on the only path that reads a backing with
    // foreign geometry. Its runtime regression test is OWED, not landed here:
    // it needs a surface + `glsrc` + `present-to` and an assertion that
    // `rb-issued` does NOT move for an undersized adoption while it DOES for
    // a correctly-sized twin -- machinery that lives in the C-6 readback
    // scenario, and a thyla-pi run to certify. Tracked, not dropped.
    let staging_pass = {
        let mut passed = false;
        // try_open_read, not open_read_string: a starved mint refuses at the
        // OPEN, and open_read_string fail()s the whole scenario there -- so the
        // "bo/new failed; the arm never ran" INSTRUMENT arm below was
        // unreachable for the one failure it names (follow-up round F5).
        let bo = try_open_read(ok, &format!("ctx/{}/bo/new", ctx_ok)).and_then(|s| parse_u32_prefix(&s));
        let bo2 = try_open_read(ok, &format!("ctx/{}/bo/new", ctx_ok)).and_then(|s| parse_u32_prefix(&s));
        match (bo, bo2) {
            (Some(a), Some(b)) => {
                // What Mesa actually emits for a staged / MSAA 512x512 BGRA
                // texture: true geometry, one page of declared backing.
                let staging = format!(
                    "create3d {} {} {} 512 512 1 1 0 0 0 4096",
                    PIPE_TEXTURE_2D, VIRGL_FORMAT_B8G8R8A8_UNORM, VIRGL_BIND_RENDER_TARGET
                );
                // Malformed for a reason the seam still owns: not page-aligned.
                let malformed = format!(
                    "create3d {} {} {} 512 512 1 1 0 0 0 4095",
                    PIPE_TEXTURE_2D, VIRGL_FORMAT_B8G8R8A8_UNORM, VIRGL_BIND_RENDER_TARGET
                );
                let took_staging = write_ctl(ok, &format!("ctx/{}/bo/{}/ctl", ctx_ok, a), &staging);
                let took_malformed =
                    write_ctl(ok, &format!("ctx/{}/bo/{}/ctl", ctx_ok, b), &malformed);
                if !took_staging {
                    t_putstr(
                        "warp-prove: C0-STAGING FAIL -- create3d REFUSED a 512x512 B8G8R8A8 BO \
                         declaring one page, which is exactly what Mesa's staging and MSAA \
                         paths emit for a legitimate texture; GL allocation is broken\n",
                    );
                } else if took_malformed {
                    t_putstr(
                        "warp-prove: C0-STAGING INSTRUMENT -- the CONTROL was admitted too; \
                         create3d is admitting an unaligned backing, so \"the staging shape \
                         was admitted\" means nothing\n",
                    );
                } else {
                    passed = true;
                    t_putstr(
                        "warp-prove: C0-STAGING PASS -- one page for 512x512 ADMITTED (the \
                         Mesa staging/MSAA shape), an unaligned backing still REFUSED (the \
                         arm discriminates; it does not merely admit)\n",
                    );
                }
            }
            _ => {
                t_putstr(
                    "warp-prove: C0-STAGING INSTRUMENT -- bo/new failed; the arm never ran\n",
                );
            }
        }
        passed
    };

    // The completion token is a VERDICT now (F6): DONE iff every arm above
    // passed; otherwise the first arm that did not, by name. The #240
    // measurement (`ANSWER=`) is data, not an arm -- it has no pass/fail --
    // and the host gate still requires it separately.
    let incomplete = if !detect_pass {
        Some("detect")
    } else if !sticky_pass {
        Some("sticky")
    } else if !f1_defended {
        Some("f1")
    } else if !staging_pass {
        Some("staging")
    } else {
        None
    };
    match incomplete {
        None => t_putstr("warp-prove: C0-REJECT DONE\n"),
        Some(arm) => t_putstr(&format!("warp-prove: C0-REJECT INCOMPLETE({})\n", arm)),
    };

    unsafe {
        t_close(bad);
        t_close(ok);
        t_close(f1);
    }
}

/// A stateless `RESOURCE_COPY_REGION` (opcode 17, 13 payload dwords) of a
/// box `w` wide (BYTES on a buffer, texels on a texture), 1 high, 1 deep,
/// origin to origin. Mirrors the server's own probe stream -- deliberately,
/// since the F1 question is whether a CLIENT can issue the same command
/// against the server's resources.
fn rcr_stream(st: &mut Vec<u32>, src_res: u32, dst_res: u32, w: u32) {
    st.push(cmd0(VIRGL_CCMD_RESOURCE_COPY_REGION, 0, VIRGL_CMD_RCR_SIZE));
    st.push(dst_res);
    st.push(0); // dst level
    st.push(0); // dst x
    st.push(0); // dst y
    st.push(0); // dst z
    st.push(src_res);
    st.push(0); // src level
    st.push(0); // src x
    st.push(0); // src y
    st.push(0); // src z
    st.push(w); // src w
    st.push(1); // src h
    st.push(1); // src d
}

/// Mint a BUFFER BO of `PROBE_BUF_BYTES` -- the shape the server's probe
/// pairs have -- and return `(bo_id, res_id, mapped_va)`.
fn mint_buffer_bo(root: i64, ctx: u32, what: &str) -> (u32, u32, u64) {
    let bo = match parse_u32_prefix(&open_read_string(root, &format!("ctx/{}/bo/new", ctx))) {
        Some(v) => v,
        None => fail(&format!("{}: bo/new", what)),
    };
    let create = format!(
        "create3d {} {} {} {} 1 1 1 0 0 0 {}",
        PIPE_BUFFER, VIRGL_FORMAT_R8_UNORM, VIRGL_BIND_VERTEX_BUFFER, PROBE_BUF_BYTES, PROBE_BUF_BYTES
    );
    if !write_ctl(root, &format!("ctx/{}/bo/{}/ctl", ctx, bo), &create) {
        fail(&format!("{}: buffer create3d", what));
    }
    let res = parse_field(&open_read_string(root, &format!("ctx/{}/bo/{}/info", ctx, bo)), "res")
        .unwrap_or_else(|| fail(&format!("{}: info `res`", what))) as u32;
    let map_fd = unsafe {
        let p = format!("ctx/{}/bo/{}/map", ctx, bo);
        t_open(root, p.as_ptr(), p.len(), T_OREAD)
    };
    if map_fd < 0 {
        fail(&format!("{}: map open", what));
    }
    let va = unsafe { t_weft_map(map_fd as u64, 0) };
    if va < 0 {
        fail(&format!("{}: weft_map", what));
    }
    (bo, res, va as u64)
}

/// A readback that REPORTS instead of failing -- `resample` aborts the run
/// when a transfer is refused, which is itself one of the outcomes this leg
/// exists to measure.
fn quiet_readback(root: i64, ctx: u32, bo: u32, va: u64) -> u32 {
    let px0 = va as *mut u32;
    unsafe { core::ptr::write_volatile(px0, SENTINEL) };
    let before = ctx_field(root, ctx, "fence-signaled");
    let xfer = format!("transfer_from 0 0 0 0 {} {} 1 0 0 0", W, H);
    if !write_ctl(root, &format!("ctx/{}/bo/{}/ctl", ctx, bo), &xfer) {
        return XFER_REFUSED;
    }
    for _ in 0..200 {
        if ctx_field(root, ctx, "fence-signaled") > before {
            break;
        }
    }
    unsafe { core::ptr::read_volatile(px0) }
}

/// The cross-client properties. Sequential by construction: every step is a
/// synchronous 9P round trip, so "while A holds" is a real program state and
/// not a window we are hoping to hit.
fn prove_two_clients() {
    let a = warp_connect("client A");
    let b = warp_connect("client B");

    let ctx_a = mint_ctx(a, "client A");
    let ctx_b = mint_ctx(b, "client B");
    // B minting AT ALL is the proof the two roots are distinct connections:
    // `wctx_mint` refuses a second ctx on a conn that already owns one ("one
    // ctx per client"), so a shared session would have failed above. Assert
    // the ids differ too -- a same-id pair would mean B was handed A's ctx,
    // which would make every leg below vacuous.
    if ctx_a == ctx_b {
        fail("two-client: A and B got the SAME ctx -- one connection, not two; every leg below would be vacuous");
    }

    // --- L1 + L3: the hold is exclusive, and only its owner may drop it ----
    // Round-8 F1: `hold_ctx` was a single global slot, so B's arm silently
    // DISPLACED A's -- stranding whatever A had already deferred. The fix
    // refuses instead; these two writes must fail.
    if !write_ctl(a, "ctl", "warp-hold on") {
        fail("two-client: A warp-hold on (is tapestryd built with test-mode?)");
    }
    if write_ctl(b, "ctl", "warp-hold on") {
        fail("two-client: B ARMED the hold while A held it (round-8 F1: displacement, \
              which strands A's deferred retires)");
    }
    if write_ctl(b, "ctl", "warp-hold off") {
        fail("two-client: B RELEASED a hold it does not own (a foreign client can end \
              another's hold -- the same displacement hole from the other side)");
    }

    // --- L4: a held lane still serves everyone else ------------------------
    // This is the property the whole #178 scoping exists to provide, and
    // nothing has ever proven it. Watched through a BOUNDED ctl poll rather
    // than the fence fd: that read parks, so a regression would hang the
    // prover and surface as a boot timeout instead of a message.
    let bo_b = mint_backed_bo(b, ctx_b, "client B");
    // Watch the MONOTONIC retire counter, not the in-flight gauge (#184).
    // `fences-in-flight == 0` is satisfied just as well by "B never queued
    // anything" as by "B's fence landed", so a no-op regression in the
    // submit path would have kept this leg green while the lane was dead.
    // `fence-signaled` only advances in the pump a swallowed retire never
    // reaches -- and unlike the L2 slot-count guard below, it cannot race
    // the retire, because B is deliberately unheld and so completes fast.
    let sig_before = ctx_field(b, ctx_b, "fence-signaled");
    if !write_ctl(
        b,
        &format!("ctx/{}/bo/{}/ctl", ctx_b, bo_b),
        "transfer_from 0 0 0 0 1 1 1 0 0 0",
    ) {
        fail("two-client: B transfer_from refused while A held the lane (the hold is \
              GLOBAL, not per-ctx -- one client can stall every other client's 3D)");
    }
    let mut retired = false;
    for _ in 0..400 {
        if ctx_field(b, ctx_b, "fence-signaled") > sig_before {
            retired = true;
            break;
        }
    }
    if !retired {
        fail("two-client: B's fence never RETIRED while A held the lane -- A's hold is \
              deferring B's completions, so any client can freeze the 3D lane box-wide");
    }
    t_putstr("warp-prove: held lane still retired a second client's fence\n");

    // --- L2: a holder that DEPARTS must not strand its slots ---------------
    // The round-8 F1 fix rests on `warp_retire_conn` releasing the hold and
    // replaying that ctx's deferred retires. `ctxs` cannot witness it --
    // warp_live_ctxs excludes `retiring` contexts, so a ctx stranded forever
    // reads exactly like one that finished (round-5 F5, one level down). The
    // slot count is the resource that actually leaks, so count that.
    let free_before = fenced_free(b);
    let bo_a = mint_backed_bo(a, ctx_a, "client A");
    if !write_ctl(
        a,
        &format!("ctx/{}/bo/{}/ctl", ctx_a, bo_a),
        "transfer_from 0 0 0 0 1 1 1 0 0 0",
    ) {
        fail("two-client: A transfer_from queue");
    }
    let free_held = fenced_free(b);
    // ANTI-VACUOUS. If A's chain already retired, the close below proves
    // nothing about replaying deferred work -- the same trap #177 caught in
    // the poisoned path, where the drain beat the abandon and 17 rounds ran
    // against a healthy ctx while reporting PASS.
    if free_held >= free_before {
        fail(&format!(
            "two-client: A's held chain did not occupy a fenced slot (free {} -> {}), so the \
             departing-holder leg would test nothing",
            free_before, free_held
        ));
    }
    unsafe { t_close(a) };
    let mut returned = false;
    for _ in 0..400 {
        if fenced_free(b) >= free_before {
            returned = true;
            break;
        }
    }
    if !returned {
        fail(&format!(
            "two-client: A's fenced slot never came back after A disconnected (free {} vs {} \
             before) -- the departing holder's deferred retires were never replayed (round-8 F1)",
            fenced_free(b),
            free_before
        ));
    }
    t_putstr("warp-prove: a departing holder's fenced slots returned to the pool\n");

    // --- L6: a holder that DESTROYS its ctx, conn still open ---------------
    // L2 above exercises conn departure -- which the round-7 placement of the
    // hold release (on `warp_retire_conn`) already handled. The round-8 F1
    // fix moved it to `wctx_retire` for a different case its own comment
    // names: a client that holds, submits, then destroys its ctx WITHOUT
    // closing the conn. There the swallowed retire kept `fences_in_flight`
    // nonzero, so the pump could never finish the ctx it had just been told
    // to retire. Nothing pinned that half until this leg (#185).
    let c = warp_connect("client C");
    let ctx_c = mint_ctx(c, "client C");
    if !write_ctl(c, "ctl", "warp-hold on") {
        fail("two-client: C warp-hold on");
    }
    let bo_c = mint_backed_bo(c, ctx_c, "client C");
    let free_before_c = fenced_free(b);
    if !write_ctl(
        c,
        &format!("ctx/{}/bo/{}/ctl", ctx_c, bo_c),
        "transfer_from 0 0 0 0 1 1 1 0 0 0",
    ) {
        fail("two-client: C transfer_from queue");
    }
    // Anti-vacuous, and sound here for the same reason it is in L2: C HOLDS,
    // so its retire is swallowed and the slot stays occupied deterministically.
    if fenced_free(b) >= free_before_c {
        fail("two-client: C's held chain did not occupy a fenced slot, so the \
              destroy-under-hold leg would test nothing");
    }
    // Destroy WITHOUT releasing -- the whole point of the leg.
    if !write_ctl(c, &format!("ctx/{}/ctl", ctx_c), "destroy") {
        fail("two-client: C ctx destroy");
    }
    let mut c_returned = false;
    for _ in 0..400 {
        if fenced_free(b) >= free_before_c {
            c_returned = true;
            break;
        }
    }
    if !c_returned {
        fail("two-client: C destroyed its ctx while holding and the fenced slot never \
              came back -- the hold release is not on the chokepoint every ctx death \
              passes through, so a client can strand a lane slot without disconnecting");
    }
    unsafe { t_close(c) };
    t_putstr("warp-prove: a holder that destroyed its ctx released the lane\n");

    // --- L5a: abandon FIRES, and takes the abandoning client's own slot ----
    // A fresh conn, because A is gone. This leg's job is the positive half:
    // the lever really abandons something, and what it abandons is its own.
    // The scoping half is L5b below -- see the note there for why the
    // "B is not poisoned" assertion in THIS leg cannot carry that claim.
    let a2 = warp_connect("client A2");
    let ctx_a2 = mint_ctx(a2, "client A2");
    if !write_ctl(a2, "ctl", "warp-hold on") {
        fail("two-client: A2 warp-hold on");
    }
    let bo_a2 = mint_backed_bo(a2, ctx_a2, "client A2");
    if !write_ctl(
        a2,
        &format!("ctx/{}/bo/{}/ctl", ctx_a2, bo_a2),
        "transfer_from 0 0 0 0 1 1 1 0 0 0",
    ) {
        fail("two-client: A2 transfer_from queue");
    }
    let ab_before = parse_field(&open_read_string(a2, "ctl"), "abandoned")
        .unwrap_or_else(|| fail("two-client: ctl `abandoned` missing"));
    if !write_ctl(a2, "ctl", "warp-abandon") {
        fail("two-client: A2 warp-abandon");
    }
    let ab_after = parse_field(&open_read_string(a2, "ctl"), "abandoned")
        .unwrap_or_else(|| fail("two-client: ctl `abandoned` missing after abandon"));
    if ab_after <= ab_before {
        fail("two-client: warp-abandon abandoned 0 NEW slots -- the scoping assertions below \
              would run against an unpoisoned seam");
    }
    match parse_field(&open_read_string(a2, &format!("ctx/{}/ctl", ctx_a2)), "poisoned") {
        Some(1) => {}
        _ => fail("two-client: A2's own ctx is not poisoned after its abandon"),
    }
    match parse_field(&open_read_string(b, &format!("ctx/{}/ctl", ctx_b)), "poisoned") {
        Some(0) => {}
        _ => fail("two-client: B's ctx was poisoned by A2's abandon -- abandon is GLOBAL, so \
                   any client can wedge every other client's context"),
    }
    // Ownership opacity, while we have two live conns to test it with: B must
    // not be able to resolve A2's ctx at all (`wctx` is conn-scoped).
    let foreign = format!("ctx/{}/ctl", ctx_a2);
    let fr = unsafe { t_open(b, foreign.as_ptr(), foreign.len(), T_OREAD) };
    if fr >= 0 {
        unsafe { t_close(fr) };
        fail("two-client: B resolved A2's ctx -- contexts are not conn-scoped");
    }
    t_putstr("warp-prove: abandon stayed inside the abandoning client\n");

    // Leave the seam clean: release, let the late retire vindicate A2, and
    // destroy both contexts. A gate that ends with the lane wedged would make
    // every later boot's clean-path assertion a lie.
    if !write_ctl(a2, "ctl", "warp-hold off") {
        fail("two-client: A2 warp-hold off");
    }
    let mut healed = false;
    for _ in 0..400 {
        let c = open_read_string(a2, &format!("ctx/{}/ctl", ctx_a2));
        if parse_field(&c, "poisoned") == Some(0) {
            healed = true;
            break;
        }
    }
    if !healed {
        fail("two-client: A2 never vindicated after the hold released");
    }
    let _ = write_ctl(a2, &format!("ctx/{}/ctl", ctx_a2), "destroy");
    let _ = write_ctl(b, &format!("ctx/{}/ctl", ctx_b), "destroy");
    unsafe { t_close(a2) };
    unsafe { t_close(b) };

    // --- L5b: abandon does not reach ACROSS clients ------------------------
    // #188. L5a's bystander (B) was unheld, so by the time the abandon ran
    // B's fence had long since retired and B owned no in-flight slot at all.
    // `abandon_matching` walks in-flight fence SLOTS -- so with none of B's to
    // walk, a GLOBAL abandon and a scoped one do exactly the same thing to B,
    // and the global shape (the pre-#178 box-wide DoS) passes L5a unchanged.
    // Proved, not supposed: sabotage S5a reverted the scoping to global and
    // the harness printed "abandon stayed inside the abandoning client".
    //
    // The fix is to put the victim's fence genuinely at risk. Only the HOLDER
    // can pin one -- the hold is what defers a chain's completion -- and
    // round-8 F1 makes the hold exclusive (L1 asserts the displacement is
    // refused), so the two roles cannot both hold. Hence: the VICTIM holds
    // (pinning its fence in flight for the whole leg) and the ABANDONER runs
    // unheld. That inverts L5a's arrangement, which is the point.
    let d = warp_connect("client D");        // victim: holds, so its fence pins
    let e = warp_connect("client E");        // abandoner: unheld
    let ctx_d = mint_ctx(d, "client D");
    let ctx_e = mint_ctx(e, "client E");

    if !write_ctl(d, "ctl", "warp-hold on") {
        fail("two-client: D warp-hold on");
    }
    let bo_d = mint_backed_bo(d, ctx_d, "client D");
    let free_before_d = fenced_free(d);
    if !write_ctl(
        d,
        &format!("ctx/{}/bo/{}/ctl", ctx_d, bo_d),
        "transfer_from 0 0 0 0 1 1 1 0 0 0",
    ) {
        fail("two-client: D transfer_from queue");
    }
    // The pin is this leg's entire premise -- assert it rather than assume it.
    // Without it there is nothing for a global abandon to steal and L5b would
    // be as blind as L5a was.
    if fenced_free(d) >= free_before_d {
        fail("two-client: D's held fence never occupied a fenced slot, so a global \
              abandon would have nothing of D's to take -- L5b would test nothing");
    }

    let ab2_before = parse_field(&open_read_string(e, "ctl"), "abandoned")
        .unwrap_or_else(|| fail("two-client: ctl `abandoned` missing (L5b)"));
    if !write_ctl(e, "ctl", "warp-abandon") {
        fail("two-client: E warp-abandon");
    }
    let ab2_after = parse_field(&open_read_string(e, "ctl"), "abandoned")
        .unwrap_or_else(|| fail("two-client: ctl `abandoned` missing after E's abandon"));

    // Three independent readings, each of which a global abandon breaks. The
    // counter is the sharpest: E holds nothing in flight, so a correctly-scoped
    // abandon can only be a no-op, while a global one must consume D's slot.
    if ab2_after != ab2_before {
        fail("two-client: E abandoned a slot it does not own -- E has NOTHING in flight, \
              so the only slot available to take was D's; abandon is GLOBAL and any client \
              can wedge every other client's in-flight work");
    }
    if ctx_field(d, ctx_d, "poisoned") != 0 {
        fail("two-client: D's ctx was poisoned by E's abandon -- abandon reaches across \
              clients, so an unprivileged peer can poison a context it does not own");
    }
    // Positive survival evidence, not merely absence of poison. A gauge is the
    // right instrument HERE precisely because D holds: the hold pins the chain,
    // so "in flight" is a stable state rather than a coin flip against a fast
    // completion (#184 -- do NOT copy this reading into an unheld context).
    if ctx_field(d, ctx_d, "fences-in-flight") == 0 {
        fail("two-client: D's pinned fence left flight across E's abandon -- D still holds, \
              so nothing of D's should have moved");
    }
    t_putstr("warp-prove: abandon did not reach a peer's PINNED in-flight fence\n");

    // Leave the lane clean: release D's hold and let its chain retire.
    if !write_ctl(d, "ctl", "warp-hold off") {
        fail("two-client: D warp-hold off");
    }
    let mut d_returned = false;
    for _ in 0..400 {
        if fenced_free(d) >= free_before_d {
            d_returned = true;
            break;
        }
    }
    if !d_returned {
        fail("two-client: D's fenced slot never came back after its hold released");
    }
    let _ = write_ctl(d, &format!("ctx/{}/ctl", ctx_d), "destroy");
    let _ = write_ctl(e, &format!("ctx/{}/ctl", ctx_e), "destroy");
    unsafe { t_close(d) };
    unsafe { t_close(e) };
}

/// Drive a ctx into the wedge state and assert the bounds that hold there.
///
/// The whole leg is deterministic by construction: `warp-hold on` stops the
/// completion drain so the submitted fence STAYS in flight, and
/// `warp-abandon` forces the transition the 30 s clock would otherwise
/// make. Shortening the clock instead would have made this a race against
/// wall time -- the flake shape the harness exists to remove.
/// One page is the minimum the seam accepts, and it is what makes the COUNT
/// cap (not the byte cap) the binding one in the poisoned churn.
fn smallest_bo_create() -> String {
    format!(
        "create3d {} {} {} 1 1 1 1 0 0 0 {}",
        PIPE_TEXTURE_2D, VIRGL_FORMAT_B8G8R8A8_UNORM, VIRGL_BIND_RENDER_TARGET, 4096
    )
}

/// Mint a ctx and wedge it: hold on, one submit, abandon. Every step is asserted,
/// and the hold is left on, so the slot stays poisoned until `warp-hold off`.
fn poisoned_ctx(root: i64, nop: &[u8]) -> u32 {
    // The levers live on the WARP ctl, not the tapestry one -- a warp
    // client has no path to the tapestry tree. #178: they act only on the
    // CALLER'S ctx, so the ctx must exist first; a lever write with no ctx
    // is E_INVAL by design, which makes a mis-sequenced test fail loudly
    // instead of silently holding nothing.
    let ctx = match parse_u32_prefix(&open_read_string(root, "ctx/new")) {
        Some(v) => v,
        None => fail("poisoned-path ctx/new"),
    };
    if !write_ctl(root, "ctl", "warp-hold on") {
        fail("warp-hold on (is tapestryd built with the test-mode feature?)");
    }
    let bo = match parse_u32_prefix(&open_read_string(root, &format!("ctx/{}/bo/new", ctx))) {
        Some(v) => v,
        None => fail("poisoned-path bo/new"),
    };
    if !write_ctl(root, &format!("ctx/{}/bo/{}/ctl", ctx, bo), &smallest_bo_create()) {
        fail("poisoned-path create3d");
    }
    let submit_path = format!("ctx/{}/submit", ctx);
    let fd = unsafe { t_open(root, submit_path.as_ptr(), submit_path.len(), T_OWRITE) };
    if fd < 0 {
        fail("poisoned-path submit open");
    }
    let _ = unsafe { t_write(fd, nop.as_ptr(), nop.len()) };
    unsafe { t_close(fd) };

    // THE ANTI-VACUOUS GATE. If the drain beat us the fence is already
    // retired, the abandon is a silent no-op, and every assertion below
    // would run against a perfectly healthy ctx and PASS -- a harness
    // reporting success for a state it never reached. Assert the trigger
    // actually bit before believing anything downstream of it.
    // Round-7 F2: assert the DELTA, not the total. `abandoned` is a
    // process-lifetime counter, so `>= 1` is satisfied by any earlier
    // abandon -- a second invocation of this leg, or a clean path that
    // ever abandons, would let the gate pass on a STALE trigger while
    // this run's abandon did nothing. The whole sub-arc has twice turned
    // on a gate that quietly stopped discriminating; read it before and
    // after and require THIS write to have moved it.
    let before = parse_field(&open_read_string(root, "ctl"), "abandoned")
        .unwrap_or_else(|| fail("ctl `abandoned` field missing (test-mode build?)"));
    if !write_ctl(root, "ctl", "warp-abandon") {
        fail("warp-abandon");
    }
    let after = parse_field(&open_read_string(root, "ctl"), "abandoned")
        .unwrap_or_else(|| fail("ctl `abandoned` field missing after abandon"));
    if after <= before {
        fail("warp-abandon abandoned 0 NEW slots -- the hold did not hold, \
              so the poisoned-path legs would have tested a healthy ctx");
    }
    let cctl = open_read_string(root, &format!("ctx/{}/ctl", ctx));
    match parse_field(&cctl, "poisoned") {
        Some(1) => {}
        _ => fail("ctx not poisoned after abandon"),
    }
    ctx
}

/// Hold a poisoned slot until `/tmp/poison-go` appears (at most `argv[2]`
/// seconds), retire one of this ctx's own BOs, then let the slot heal and say so.
/// Nothing here judges the retirement a second client makes meanwhile; the
/// scenario judges this one against the broker's own refusal line.
fn poison_hold() -> i64 {
    let secs: u64 = libthyla_rs::env::args()
        .get_str(2)
        .and_then(|s| s.parse().ok())
        .unwrap_or_else(|| fail("poison-hold <seconds>"));
    let root = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv/warp".as_ptr(), 9, T_OREAD) };
    if root < 0 {
        fail("poison-hold: open /srv/warp");
    }
    let nop = [cmd0(VIRGL_CCMD_CLEAR, 0, 0).to_le_bytes()].concat();
    let ctx = poisoned_ctx(root, &nop);
    t_putstr(&format!("warp-prove: POISON-HOLD READY ctx {} for {} s\n", ctx, secs));
    let t0 = libthyla_rs::time::Instant::now();
    loop {
        let go = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/tmp/poison-go".as_ptr(), 14, T_OREAD) };
        if go >= 0 {
            unsafe { t_close(go) };
            break;
        }
        if t0.elapsed().as_secs() >= secs {
            fail("poison-hold: never told to retire its own BO");
        }
        let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(200));
    }
    let bo = match parse_u32_prefix(&open_read_string(root, &format!("ctx/{}/bo/new", ctx))) {
        Some(v) => v,
        None => fail("poison-hold: bo/new"),
    };
    let bo_ctl = format!("ctx/{}/bo/{}/ctl", ctx, bo);
    if !write_ctl(root, &bo_ctl, &smallest_bo_create()) {
        fail("poison-hold: create3d on the poisoned ctx");
    }
    let leaked = |c: &str| {
        parse_field(c, "leaked-count").unwrap_or_else(|| fail("poison-hold: no leaked-count"))
    };
    let before = leaked(&open_read_string(root, &format!("ctx/{}/ctl", ctx)));
    let t1 = libthyla_rs::time::Instant::now();
    if !write_ctl(root, &bo_ctl, "destroy") {
        fail("poison-hold: destroy its own BO");
    }
    let ms = t1.elapsed().as_millis();
    let after = leaked(&open_read_string(root, &format!("ctx/{}/ctl", ctx)));
    if after != before + 1 {
        fail("poison-hold: its own BO was not leak-parked, so the slot did not count");
    }
    t_putstr(&format!("warp-prove: POISON-HOLD OWN-RETIRE ms={}\n", ms));
    let c = open_read_string(root, &format!("ctx/{}/ctl", ctx));
    if parse_field(&c, "poisoned") != Some(1) {
        fail("poison-hold: the slot healed during the hold, so the timing beside it proves nothing");
    }
    if !write_ctl(root, "ctl", "warp-hold off") {
        fail("poison-hold: warp-hold off");
    }
    for _ in 0..400 {
        let c = open_read_string(root, &format!("ctx/{}/ctl", ctx));
        if parse_field(&c, "poisoned") == Some(0) {
            t_putstr("warp-prove: POISON-HOLD RELEASED\n");
            return 0;
        }
    }
    fail("poison-hold: the poison never cleared after the hold released")
}

fn prove_poisoned_path(root: i64) {
    let nop = [cmd0(VIRGL_CCMD_CLEAR, 0, 0).to_le_bytes()].concat();
    let ctx = poisoned_ctx(root, &nop);
    let submit_path = format!("ctx/{}/submit", ctx);
    let small = smallest_bo_create();

    // R5-F2: poison is TERMINAL to the client. Submit must refuse rather
    // than let the stream be re-armed over backings the device may still
    // be writing.
    let fd2 = unsafe { t_open(root, submit_path.as_ptr(), submit_path.len(), T_OWRITE) };
    if fd2 >= 0 {
        let n = unsafe { t_write(fd2, nop.as_ptr(), nop.len()) };
        unsafe { t_close(fd2) };
        if n >= 0 {
            fail("submit SUCCEEDED on a poisoned ctx (R5-F2 terminal poison)");
        }
    }

    // THE R6-F1 REGRESSION ASSERTION. Churn mint -> create3d -> destroy.
    // Each destroy on a poisoned ctx leak-parks a backing. Post-fix the
    // creation-time cap (leaked_count + live_backed >= MAX_WARP_BOS_PER_CTX)
    // refuses by attempt cap+1. PRE-fix nothing capped the count, so this
    // loop ran to the BYTE cap -- 64 MiB / 4 KiB = 16384 attempts -- silently
    // dropping every WarpBo past the cap-wide graveyard and leaking a kernel
    // handle plus a mapping with each one. So "refused by cap+1" is the
    // discriminator; "eventually refused" is true BOTH ways and would be a
    // gate that cannot fail. The cap is READ from ctl `bo-cap` (Warp-3),
    // not hardcoded: a mirrored constant is a claimed sync with no check
    // (#187), and the server lifting the width must move this bound with it
    // or the discriminator quietly widens toward "eventually".
    let cap_line = open_read_string(root, "ctl");
    let cap: u32 = match parse_field(&cap_line, "bo-cap") {
        Some(v) if v >= 1 && v <= 4096 => v as u32,
        Some(v) => fail(&format!("ctl bo-cap {} implausible", v)),
        None => fail("ctl `bo-cap` field missing (pre-Warp-3 tapestryd?)"),
    };
    let mut refused_at = 0u32;
    // The failure message carries the per-round (poisoned, leaked-count)
    // trace, so ONE red run says WHICH way it broke rather than only that
    // it broke: a climbing count with no refusal indicts the gate, a count
    // stuck at 0 indicts the leak accounting, and poisoned flipping to 0
    // mid-loop indicts an unexpected vindication. A bare "was never
    // refused" sends you back for a second run to learn that.
    let mut trace = String::new();
    for i in 1..=(cap + 1) {
        // THE PRECONDITION, RE-ASSERTED PER ROUND (#177). The pre-loop
        // anti-vacuous gate proves the trigger FIRED; it says nothing
        // about whether the state it created SURVIVES the assertions. It
        // did not: `submit_and_wait` drains un-gated, so the first
        // create3d vindicated the ctx and every round below ran against a
        // healthy one -- correctly never refusing, and reported as the
        // cap being broken. A precondition checked once is a precondition
        // checked before the code that voids it.
        let pre = open_read_string(root, &format!("ctx/{}/ctl", ctx));
        if parse_field(&pre, "poisoned") != Some(1) {
            fail(&format!(
                "ctx un-poisoned before churn round {} -- the wedge did not persist, so a \
                 refusal (or its absence) proves NOTHING about the round-6 cap; trace:{}",
                i, trace
            ));
        }
        let b = match parse_u32_prefix(&open_read_string(root, &format!("ctx/{}/bo/new", ctx))) {
            Some(v) => v,
            None => {
                refused_at = i;
                break;
            }
        };
        if !write_ctl(root, &format!("ctx/{}/bo/{}/ctl", ctx, b), &small) {
            refused_at = i;
            break;
        }
        if !write_ctl(root, &format!("ctx/{}/bo/{}/ctl", ctx, b), "destroy") {
            fail("poisoned-path bo destroy");
        }
        let c = open_read_string(root, &format!("ctx/{}/ctl", ctx));
        trace.push_str(&format!(
            " {}:p{}l{}",
            i,
            parse_field(&c, "poisoned").unwrap_or(9),
            parse_field(&c, "leaked-count").unwrap_or(999)
        ));
    }
    if refused_at == 0 {
        fail(&format!(
            "BO creation was never refused in cap+1 poisoned-churn rounds -- \
             the leak count is unbounded (round-6 F1); trace(round:poisoned,leaked-count):{}",
            trace
        ));
    }
    t_putstr(&format!(
        "warp-prove: poisoned churn refused at attempt {} (cap {})\n",
        refused_at, cap
    ));

    // Release the hold: the held completion now drains, the late retire
    // proves the device finished, and the vindication must walk the ctx
    // back out of the wedge -- freeing the graveyard and resetting the
    // counters (round-6, the live-vindication reclamation point).
    if !write_ctl(root, "ctl", "warp-hold off") {
        fail("warp-hold off");
    }
    // Bounded, and self-pacing: each ctl read is a 9P round trip, so the
    // loop body itself forces a serve-loop pass -- no sleep primitive
    // needed, and no unbounded waiter.
    let mut healed = false;
    for _ in 0..400 {
        let c = open_read_string(root, &format!("ctx/{}/ctl", ctx));
        if parse_field(&c, "poisoned") == Some(0) && parse_field(&c, "leaked-count") == Some(0) {
            healed = true;
            break;
        }
    }
    if !healed {
        fail("vindication never cleared the poison / leak counters after the hold released");
    }
    t_putstr("warp-prove: vindication reclaimed the wedged ctx\n");

    if !write_ctl(root, &format!("ctx/{}/ctl", ctx), "destroy") {
        fail("poisoned-path ctx destroy");
    }
    let ctl3 = open_read_string(root, "ctl");
    match parse_field(&ctl3, "poisoned") {
        Some(0) => {}
        Some(v) => fail(&format!("poisoned {} after vindication+destroy (want 0)", v)),
        None => fail("ctl poisoned field missing"),
    }
    let _ = write_ctl(root, "ctl", "warp-hold off");
}

/// #240 audit F3: the probe graveyard must RECLAIM. The wedge posture leaks
/// a ctx's probe backings deliberately -- the device may still be mid-DMA
/// into those pages -- but a leak with no reclaim is a permanent handle burn
/// on the process that IS the console, and an unprivileged client can drive
/// one per FENCE_ABANDON_MS. BOs have been leak-then-reclaim since round 5;
/// the probe was leak-and-FORGET while the reference doc claimed it shared
/// their posture, which is the respect that actually bounds the damage.
///
/// Two claims, so two assertions, each falsifiable ALONE (#185): the destroy
/// PARKS (delete the park -> `probe-parked` never moves) and the vindication
/// FREES (delete the free -> `probe-freed` never moves). Both counters are
/// monotonic and read BEFORE the cycle, because a live gauge reading 0 is
/// equally satisfied by "the path never ran" (#184).
///
/// What this does NOT prove: that the freed handles are genuinely gone.
/// Nothing in the tree can count a Proc's open fds (#234), so the ledger is
/// the producer REPORTING its own act, not an independent audit of it. The
/// slot-poison census is folded in as a second, independent reading of the
/// same vindication event.
fn prove_probe_reclaim(root: i64) {
    let g0 = open_read_string(root, "ctl");
    let parked0 = parse_field(&g0, "probe-parked")
        .unwrap_or_else(|| fail("probe-reclaim: ctl `probe-parked` missing (pre-F3 tapestryd?)"));
    let freed0 = parse_field(&g0, "probe-freed")
        .unwrap_or_else(|| fail("probe-reclaim: ctl `probe-freed` missing"));
    let poisoned0 = parse_field(&g0, "poisoned")
        .unwrap_or_else(|| fail("probe-reclaim: ctl `poisoned` missing"));

    let ctx = mint_ctx(root, "probe-reclaim");
    // PRECONDITION: this ctx actually HAS a probe. A mint whose probe failed
    // to build stores `None`, parks nothing, and would leave every assertion
    // below testing an absent object -- the leg would report the leak closed
    // because there was never anything to leak. Asked while the ctx is still
    // quiesced, which is also the only state a verify may safely run in.
    let wrote_v = write_ctl(root, &format!("ctx/{}/ctl", ctx), "verify");
    let vok = ctx_field(root, ctx, "verify-ok");
    let vseq = ctx_field(root, ctx, "verify-seq");
    t_putstr(&format!(
        "warp-prove: probe-reclaim ctx {} baseline verify wrote {} seq {} ok {} \
         (parked0 {} freed0 {} poisoned0 {})\n",
        ctx, wrote_v as u32, vseq, vok, parked0, freed0, poisoned0
    ));
    if !wrote_v || vok == 0 {
        fail("probe-reclaim: the fresh ctx has no working probe (see trace above) -- \
              nothing for the graveyard to hold, so every assertion below is vacuous");
    }

    // The wedge: hold the completion, submit, abandon. The abandon delta is
    // the anti-vacuous gate on the poison itself (#175 / round-7 F2) -- if a
    // drain beat us the ctx is HEALTHY, its destroy takes the clean arm, and
    // that arm frees the probe outright and never parks it.
    if !write_ctl(root, "ctl", "warp-hold on") {
        fail("probe-reclaim: warp-hold on (is tapestryd built with test-mode?)");
    }
    let submit_path = format!("ctx/{}/submit", ctx);
    let fd = unsafe { t_open(root, submit_path.as_ptr(), submit_path.len(), T_OWRITE) };
    if fd < 0 {
        fail("probe-reclaim: submit open");
    }
    let nop = [cmd0(VIRGL_CCMD_CLEAR, 0, 0).to_le_bytes()].concat();
    let _ = unsafe { t_write(fd, nop.as_ptr(), nop.len()) };
    unsafe { t_close(fd) };
    let ab0 = parse_field(&open_read_string(root, "ctl"), "abandoned")
        .unwrap_or_else(|| fail("probe-reclaim: ctl `abandoned` missing (test-mode build?)"));
    if !write_ctl(root, "ctl", "warp-abandon") {
        fail("probe-reclaim: warp-abandon");
    }
    let ab1 = parse_field(&open_read_string(root, "ctl"), "abandoned")
        .unwrap_or_else(|| fail("probe-reclaim: ctl `abandoned` missing after abandon"));
    if ab1 <= ab0 {
        fail("probe-reclaim: warp-abandon abandoned 0 NEW slots -- the ctx is healthy, \
              so its destroy would take the CLEAN arm and the park under test never runs");
    }

    // The destroy is what parks: a poisoned ctx retires with leak=true.
    if !write_ctl(root, &format!("ctx/{}/ctl", ctx), "destroy") {
        fail("probe-reclaim: ctx destroy");
    }
    let g1 = open_read_string(root, "ctl");
    let parked1 = parse_field(&g1, "probe-parked").unwrap_or(0);
    let poisoned1 = parse_field(&g1, "poisoned").unwrap_or(999);
    t_putstr(&format!(
        "warp-prove: probe-reclaim after wedged destroy: probe-parked {}->{} poisoned {}->{}\n",
        parked0, parked1, poisoned0, poisoned1
    ));
    if parked1 != parked0 + 1 {
        fail("probe-reclaim: a WEDGED ctx destroy did not PARK the probe (see trace above) \
              -- it was dropped, stranding two kernel handles and two mappings for the \
              life of the Proc with no vindication able to reach them (audit F3)");
    }

    // NO `warp-hold off` HERE, and its absence is load-bearing. The destroy
    // above is ALSO what releases the hold -- round-8 F1 moved the release
    // onto `wctx_retire`, the one chokepoint every ctx death passes through
    // -- so the held completion drains and the vindication follows with no
    // further lever. Writing the verb anyway is what the first cut of this
    // leg did, and it failed loudly BY DESIGN: `warp-hold` requires the
    // caller to own a LIVE ctx (#178, so a mis-sequenced test cannot
    // silently no-op), and this conn's ctx is gone.
    //
    // Bounded + self-pacing -- each ctl read is a 9P round trip, so the loop
    // body itself forces a serve-loop pass. Empirically the whole cycle
    // completes within the destroy plus one read (the slot is already
    // recovered by the trace above); the poll exists so a REGRESSION that
    // strands the parked probe times out here with its numbers instead of
    // hanging.
    let mut freed = freed0;
    let mut poisoned_now = poisoned0 + 1;
    let mut iters = 0u32;
    for i in 0..400 {
        iters = i + 1;
        let g = open_read_string(root, "ctl");
        freed = parse_field(&g, "probe-freed").unwrap_or(0);
        poisoned_now = parse_field(&g, "poisoned").unwrap_or(999);
        if freed >= freed0 + 1 && poisoned_now == poisoned0 {
            break;
        }
    }
    // THE EVIDENCE GOES BEFORE THE VERDICT, ALWAYS. The harness aborts the
    // scenario the instant it sees the hard-fail token, TRUNCATING the rest
    // of that line -- so a diagnosis carried inside the `fail()` string is
    // destroyed by the very thing it is meant to explain. (Measured: a red
    // run printed exactly `warp-prove: FAIL -- probe-r`.) Trace first as an
    // ordinary line, then fail short.
    t_putstr(&format!(
        "warp-prove: probe-reclaim trace parked {}->{} freed {}->{} poisoned {}->{} \
         after {} polls\n",
        parked0, parked1, freed0, freed, poisoned0, poisoned_now, iters
    ));
    if freed != freed0 + 1 || poisoned_now != poisoned0 {
        fail("probe-reclaim: vindication did not reclaim the parked probe (see trace above)");
    }
    t_putstr(&format!(
        "warp-prove: probe graveyard parked+reclaimed (parked {} -> {}, freed {} -> {})\n",
        parked0, parked1, freed0, freed
    ));
}

/// THE #218 REGRESSION ASSERTION. A refused create3d must consume its mint
/// record. cap+1 refusals PER FAMILY (#185: the fix has two halves -- the
/// `wbo_create` validation arms and the pre-parse arms that never reach it
/// -- and a shared loop of (cap+1)/2 each would pass with either half
/// broken). The discriminator is the MINT surviving attempt cap+1: pre-fix
/// the corpses exhaust `bos[]` and bo/new starves exactly there; any
/// smaller bound is a gate that cannot fail. The cap is READ from ctl
/// `bo-cap` (the churn-leg precedent -- a mirrored constant is a claimed
/// sync with no check, #187).
fn prove_corpse_reclaim(root: i64) {
    let cap: u32 = match parse_field(&open_read_string(root, "ctl"), "bo-cap") {
        Some(v) if v >= 1 && v <= 4096 => v as u32,
        Some(v) => fail(&format!("corpse-reclaim: ctl bo-cap {} implausible", v)),
        None => fail("corpse-reclaim: ctl `bo-cap` field missing"),
    };
    let ctx = mint_ctx(root, "corpse-reclaim");
    // Family A is refused INSIDE wbo_create (unaligned size -- the first
    // validation arm, no device traffic); family B never reaches it (a
    // truncated argument list dies at the parse).
    let bad_size = format!(
        "create3d {} {} {} 1 1 1 1 0 0 0 {}",
        PIPE_TEXTURE_2D, VIRGL_FORMAT_B8G8R8A8_UNORM, VIRGL_BIND_RENDER_TARGET, 4097
    );
    for (family, bad) in [("size-align", bad_size.as_str()), ("parse", "create3d 2")] {
        for i in 1..=(cap + 1) {
            // Tolerant read: a starved mint refuses at the OPEN, and this
            // failure message -- the family and the attempt -- IS the leg's
            // diagnosis (the generic open fail sent a red run hunting blind).
            let b = match try_open_read(root, &format!("ctx/{}/bo/new", ctx))
                .as_deref()
                .and_then(parse_u32_prefix)
            {
                Some(v) => v,
                None => fail(&format!(
                    "corpse-reclaim: mint starved at {} attempt {} (cap {}) -- refused \
                     create3ds are leaving corpses in bos[] (#218)",
                    family, i, cap
                )),
            };
            // The anti-vacuous half: the refusal must actually FIRE. A
            // server that ACCEPTS the bad create3d is not exercising the
            // corpse path, and the loop would pass around the bug.
            if write_ctl(root, &format!("ctx/{}/bo/{}/ctl", ctx, b), bad) {
                fail(&format!(
                    "corpse-reclaim: {} create3d round {} was ACCEPTED (want refusal)",
                    family, i
                ));
            }
        }
    }
    // The ctx must be fully functional after 2*(cap+1) refusals -- and the
    // unmint's UNBUILT guard must hold: a repeated create3d on a BUILT bo
    // is refused (once per BO) but must NOT unmint the live record.
    let bo = match try_open_read(root, &format!("ctx/{}/bo/new", ctx))
        .as_deref()
        .and_then(parse_u32_prefix)
    {
        Some(v) => v,
        None => fail("corpse-reclaim: good bo/new refused after the refusal loops"),
    };
    let good = format!(
        "create3d {} {} {} 1 1 1 1 0 0 0 {}",
        PIPE_TEXTURE_2D, VIRGL_FORMAT_B8G8R8A8_UNORM, VIRGL_BIND_RENDER_TARGET, 4096
    );
    let bo_ctl = format!("ctx/{}/bo/{}/ctl", ctx, bo);
    if !write_ctl(root, &bo_ctl, &good) {
        fail("corpse-reclaim: good create3d refused after the refusal loops");
    }
    if write_ctl(root, &bo_ctl, &good) {
        fail("corpse-reclaim: repeat create3d on a built bo was ACCEPTED");
    }
    let c = open_read_string(root, &format!("ctx/{}/ctl", ctx));
    if parse_field(&c, "bo-live") != Some(1) {
        fail(&format!(
            "corpse-reclaim: bo-live != 1 after the repeat-create3d refusal -- the unmint \
             touched a BUILT record (ctl: {})",
            c.trim()
        ));
    }
    if !write_ctl(root, &bo_ctl, "destroy") {
        fail("corpse-reclaim: good-bo destroy");
    }
    if !write_ctl(root, &format!("ctx/{}/ctl", ctx), "destroy") {
        fail("corpse-reclaim: ctx destroy");
    }
    t_putstr(&format!(
        "warp-prove: corpse-reclaim -- {} refused create3ds left the mint alive (cap {})\n",
        2 * (cap + 1),
        cap
    ));
}

// --- Warp-C C-6: the compositor readback arm (GPU-DESIGN 4.5.13) -----------
//
// `warp-prove readback`. The composed-GL present's READBACK arm -- taken for
// every adopted BO the blit arm cannot compose -- used to pull the frame
// host->guest SYNCHRONOUSLY on the console's dispatch thread, so the console
// waited for everything the client had queued ahead of it (C-0d Fable F2).
// C-6 makes it a fenced readback with a DEFERRED completion. This leg
// constructs the state F2 named (a non-composable BO adopted by a hosted
// surface; the owning ctx with a DEEP queue) and asserts what C-6 actually
// changes, each arm named so INCOMPLETE(<arm>) can say which:
//
//   ARM       the arm is the one under test: a present of the adoption on an
//             idle queue ISSUES a compositor readback and it LANDS (`comp-rb`
//             on the warp ctl); a composable BO would take the blit arm and
//             prove nothing (`composed gpu` moving instead is INCOMPLETE).
//   DEEP      the positive control that the queue was deep, per round: the
//             readback the device paid waited >= RB_DEEP_MS (`cost
//             readback-wait`) AND observed the queue's LAST draw (the BLUE
//             byte of the pixel it landed names the draw index). Without it
//             LIVE below is satisfied by a light queue.
//   LIVE      the console's dispatch did not wait for the readback: per
//             round, the present that ISSUES the compositor readback of a
//             deep queue returns inside RB_LIVE_MS. Under the pre-C-6 arm
//             that present returns after the whole wait. The presents and
//             ctl reads DURING the flight are REPORTED (FLIGHT REPORT), not
//             judged: another client's sync step in that window inherits the
//             device stall on QEMU/virgl and the single-threaded loop waits
//             there for everyone (4.5.13 consequence 2 -- the console
//             renderer's cursor blink is one such step; run 4 measured it).
//   DEADLINE  a client's OWN readback of its busy BO, then one more submit
//             in a colour no queue index uses, then presents of the
//             bystander surface B (operator vote 81, GPU-DESIGN 4.5.13).
//             PARKED: Lictor held the readback back until the ctx's queue
//             retired and QEMU then decoded it at once (the tag's park and
//             run, on the ctx ctl). ORDER: it landed the queue's last draw,
//             not the later submit's colour. BOUND: B's sync steps wait on
//             the queue through their own fences, so one stalls at
//             SUBMIT_DEADLINE_MS and is refused, and none outlasts
//             RB_F2B_BOUND_MS (vote 72a, which a readback holding QEMU's
//             lock made inert). HEALTH: the engine revives and nothing is
//             poisoned.
//   F2B       REPORTED, not judged: B's present latency behind that stall
//             (max / mean over the presents), the number that Venus / v3d
//             will remove and that no guest-side change can (4.5.13).
//   GUARD     an adoption of a BO declaring more than its backing issues no
//             readback; a well-backed control issues one.
//   ABANDON   a readback parked behind chains then abandoned is released, its
//             context's count never passes it, and its stream stays over.
//   CREATE    a device-context create a stall meets (4.5.15) condemns its slot
//             and owes its id a destroy; once the engine lives the destroy runs,
//             the slot comes back, and a mint takes it. Here because this leg
//             drives the GL lane's engine.
//   DESTROY   a ctx destroy a stall meets is owed, not final: the broker keeps
//             the id destroyable, the owed destroy runs once the engine lives,
//             and a mint takes the slot's id again.
//   FREE      a guest-backed BO's free a stall meets is owed by the broker,
//             which repeats it once the engine lives; the repeat is
//             acknowledged (the device answers a free that ran with no such
//             resource), the debt paid and the id retired.
//   TAKEN     a BO's free a poisoned chain can reach is taken by the broker as
//             a debt, left unpaid while the chain is poisoned, and paid once
//             the vindication retires it: links detached, freed, id retired.
//   BUSY      the same free, still owed once the ctx is live again (the broker
//             told to pass over it for a while), is paid although the ctx's
//             next chain is held: that chain parks behind the free.
//   CLEAN     after teardown nothing is poisoned and every slot is back.
//
// Prints `C6-READBACK DONE` iff every verdict arm passed, else
// `C6-READBACK INCOMPLETE(<arm>)`; warp-readback.exp hard-fails on the
// latter and on the prover's own `FAIL --`.

const VIRGL_RESOURCE_Y_0_TOP: u32 = 1 << 0;
const RB_W: u32 = 512;
const RB_H: u32 = 512;
/// Why the heavy load is DRAWS on the client's context -- three Pi runs'
/// worth of finding. Run 1 queued 800 1:1 NEAREST full-frame blits: they
/// "retired" in 16 ms -- vrend takes the `glCopyImageSubData` shortcut for
/// that shape (1.1.0 `vrend_renderer_blit`), not GPU work the readback
/// waits on. Run 2 made them SCALED: 8 submits retired in 1335 ms, real
/// work -- and the compositor readback of the same BO waited 84 ms, because
/// a scaled blit runs on vrend's separate BLITTER GL context
/// (`vrend_blitter.c`) and neither the client-context fences nor a
/// client-context `glReadPixels` are ordered behind another context. Run 3
/// alternated full-surface CLEARS between two framebuffers: the readback
/// observed the LAST clear (the mechanism is right) but 1280 clears retired
/// in 122 ms -- mesa v3d keys jobs by framebuffer (`v3d_get_job`), an FBO
/// switch does not flush, and the clears folded into two jobs. Only draws
/// cannot be elided, folded, or moved off the context -- and a real
/// client's queue IS draws.
/// The deep-queue witness floor (ms): a compositor readback that waited less
/// than this read a queue that was not deep, and LIVE proved nothing.
const RB_DEEP_MS: u64 = 100;
/// The dispatch budget (ms) while a readback is in flight: a present or a
/// ctl read answered slower than this WAITED for the device.
const RB_LIVE_MS: u64 = 50;
/// Full-screen-triangle DRAWS per heavy submit (SET_CONSTANT_BUFFER + DRAW_VBO
/// = 20 dwords each; 300 = 24 KiB, inside one Twrite) and the submits per
/// phase. `WARP_CTX_FENCE_MAX` = 8 is the share, so LIVE queues 8 (the
/// compositor readback rides the reserved slot, outside the share) and
/// DEADLINE queues 7 + the client's own readback.
const RB_DRAWS_PER_SUBMIT: usize = 300;
/// Full-screen triangles per draw (see the vertex-buffer comment). Six, and
/// FOUR submits rather than eight: the same GPU depth (~1.2 s on V3D) with
/// half the Twrites -- the send phase is the leg's exposure to another
/// client's present landing while the queue is deep (see `constructed`).
const RB_TRIS: usize = 6;
/// The heavy submits `chain-hold` keeps on the device: the C-6 gate's queue, about
/// a second of V3D.
const CHAIN_HOLD_DEPTH: u64 = 4;
/// `chain-hold deep`: every fenced slot a ctx has, each draw instanced, so the queue
/// outlasts the kernel's five-second deadline on V3D and each submit still fits a Twrite.
const CHAIN_HOLD_DEEP_DEPTH: u64 = 8;
const CHAIN_HOLD_DEEP_INSTANCES: u32 = 5;
/// The draw-state object handles (surfaces 1 and 2 are the clear legs').
const RB_H_VS: u32 = 10;
const RB_H_FS: u32 = 11;
const RB_H_VE: u32 = 12;
const RB_H_BLEND: u32 = 13;
const RB_H_DSA: u32 = 14;
const RB_H_RS: u32 = 15;
const VIRGL_CCMD_BIND_OBJECT: u32 = 2;
const VIRGL_CCMD_SET_VIEWPORT_STATE: u32 = 4;
const VIRGL_CCMD_SET_VERTEX_BUFFERS: u32 = 6;
const VIRGL_CCMD_DRAW_VBO: u32 = 8;
const VIRGL_CCMD_SET_CONSTANT_BUFFER: u32 = 12;
const VIRGL_CCMD_BIND_SHADER: u32 = 31; // SET_SUB_CTX 28, CREATE 29, DESTROY 30, BIND_SHADER 31
const VIRGL_OBJECT_BLEND: u32 = 1;
const VIRGL_OBJECT_RASTERIZER: u32 = 2;
const VIRGL_OBJECT_DSA: u32 = 3;
const VIRGL_OBJECT_SHADER: u32 = 4;
const VIRGL_OBJECT_VERTEX_ELEMENTS: u32 = 5;
const VIRGL_SHADER_VERTEX: u32 = 0;
const VIRGL_SHADER_FRAGMENT: u32 = 1;
const VIRGL_FORMAT_R32G32B32A32_FLOAT: u32 = 31;
const MESA_PRIM_TRIANGLES: u32 = 4;
const RB_LIVE_SUBMITS: usize = 4;
/// LIVE/DEEP rounds: one issuing present per deep queue, repeated; a round
/// whose sends were blocked (the readback issued with less than the floor
/// of queue left) is UNCONSTRUCTED and retried, up to RB_LIVE_ATTEMPTS.
const RB_LIVE_ROUNDS: usize = 3;
const RB_LIVE_ATTEMPTS: usize = 7;
const RB_DEADLINE_SUBMITS: usize = 6;
/// A DEADLINE readback must have waited in Lictor at least this long: its queue
/// was deep, so the parking rule had something to hold it behind (ms).
const RB_PARK_FLOOR_MS: u64 = 400;
/// Publication to retirement for a readback whose work was done: the decode of
/// a 1 MiB transfer plus the fence's round trip. One QEMU waited on takes the
/// queue (ms).
const RB_EXEC_BOUND_MS: u64 = 150;
/// The later submit's draws: RED, which no queue index paints (they paint GREEN
/// plus an index in BLUE).
const RB_LATE_DRAWS: usize = 50;
/// ABANDON's queue: long enough on V3D that every chain is still running when
/// the readback arrives and when the abandon lands a few ms later.
const RB_ABANDON_SUBMITS: usize = 3;
/// The seam's E_IO, as a refused write returns it (negated).
const E_IO_RC: i64 = 5;
/// A leg whose sends met an engine stall (another client's sync step behind the
/// queue) may have had its readback publish unparked; it retries this often.
const RB_DEADLINE_ATTEMPTS: u32 = 4;
const RB_ABANDON_ATTEMPTS: u32 = 4;
/// Lictor's SUBMIT_DEADLINE_MS (500) with slack: past it, a bystander present stalled at
/// its bound rather than waiting behind a readback (vote 72a).
const RB_F2B_BOUND_MS: u64 = 900;
/// Bound on waiting for a readback to land: FENCE_ABANDON_MS is 30 s.
const RB_LAND_BOUND_MS: u64 = 35_000;
/// Aurora paints every say within a FRAME of it (67 ms at the idle 15 Hz), and
/// tapestryd says its idle throttle up to two present buckets (2 x 250 ms)
/// after a burst: this long with no present served, nothing said is still to
/// be painted (ms).
const RB_SETTLE_QUIET_MS: u64 = 700;
/// Aurora's cursor flips every 30 FRAME events: 2 s at the idle 15 Hz (ms).
const RB_SETTLE_EDGE_MS: u64 = 2500;
/// The quiet is waited on this long at most; a console that never goes quiet
/// leaves the attempt to the construction rule (ms).
const RB_SETTLE_BOUND_MS: u64 = 6000;

/// Read a ctl file whole (offset-continued): the tapestry ctl's cost census
/// runs past the 512-byte single read the other legs take.
fn open_read_all(root: i64, path: &str) -> String {
    let fd = unsafe { t_open(root, path.as_ptr(), path.len(), T_OREAD) };
    if fd < 0 {
        fail(&format!("readback: open {} for read", path));
    }
    let mut out: Vec<u8> = Vec::new();
    let mut buf = [0u8; 512];
    for _ in 0..16 {
        let n = unsafe { t_read(fd, buf.as_mut_ptr(), buf.len()) };
        if n <= 0 {
            break;
        }
        out.extend_from_slice(&buf[..n as usize]);
        if (n as usize) < buf.len() {
            break;
        }
    }
    unsafe { t_close(fd) };
    String::from_utf8_lossy(&out).into_owned()
}

/// `cost <kind> n sum_us max_us` off the tapestry ctl.
fn cost_line(s: &str, kind: &str) -> Option<(u64, u64, u64)> {
    let key = format!("cost {} ", kind);
    for line in s.lines() {
        if let Some(rest) = line.strip_prefix(key.as_str()) {
            let mut it = rest.split_ascii_whitespace();
            let n = it.next()?.parse().ok()?;
            let sum = it.next()?.parse().ok()?;
            let max = it.next()?.parse().ok()?;
            return Some((n, sum, max));
        }
    }
    None
}

/// The two-token census `composed gpu N cpu M`.
fn composed_census(s: &str) -> Option<(u64, u64)> {
    for line in s.lines() {
        if let Some(rest) = line.strip_prefix("composed gpu ") {
            let mut it = rest.split_ascii_whitespace();
            let g = it.next()?.parse().ok()?;
            if it.next()? != "cpu" {
                return None;
            }
            let c = it.next()?.parse().ok()?;
            return Some((g, c));
        }
    }
    None
}

/// The tapestry cost census counts that name OTHER console work: presents
/// by arm (slot / cpu / bo), transfers, pushes, flushes, health steps.
fn others_census(s: &str) -> [u64; 7] {
    let n = |k: &str| cost_line(s, k).map_or(0, |(n, _, _)| n);
    [
        n("present-composed-slot"),
        n("present-composed-cpu"),
        n("present-composed-bo"),
        n("xfer"),
        n("push"),
        n("flush"),
        n("health"),
    ]
}

struct RbCensus {
    issued: u64,
    landed: u64,
    coalesced: u64,
    abandoned: u64,
    slot: u64,
}

fn rb_census(warp: i64) -> Option<RbCensus> {
    let s = open_read_all(warp, "ctl");
    Some(RbCensus {
        issued: parse_field(&s, "rb-issued")?,
        landed: parse_field(&s, "rb-landed")?,
        coalesced: parse_field(&s, "rb-coalesced")?,
        abandoned: parse_field(&s, "rb-abandoned")?,
        slot: parse_field(&s, "rb-slot")?,
    })
}

/// A readback parks behind its context's heavy queue, the queue is abandoned
/// under the hold (so no late retirement can free it), and Lictor must release
/// the readback rather than keep it parked: a parked chain has no abandonment
/// clock, and one left behind counts in the lane until the context dies,
/// holding the console's poll at 1 ms. Its own connection, because the hold and
/// the abandon act only on the caller's ctx. Leaves the lane healed for CLEAN.
fn rb_abandoned_prefix(tap: i64) -> bool {
    for k in 1..=RB_ABANDON_ATTEMPTS {
        if let Some(ok) = rb_abandon_attempt(k, tap) {
            return ok;
        }
    }
    t_putstr(&format!(
        "warp-prove: C6-RB ABANDON UNCONSTRUCTED -- in {} attempts the readback was never parked when the abandon landed\n",
        RB_ABANDON_ATTEMPTS
    ));
    false
}

/// The stall shots CREATE, DESTROY, FREE, YIELD and APART may spend each: a
/// fifth of tapestryd's TEST_STALL_SHOTS.
const RB_STALL_ATTEMPTS: u32 = 4;
/// How soon the broker repeats a free after a stall its pass did not meet: at
/// the pass's next turn. A pass that stood down after every stall would wait
/// a window first (Lictor's OWED_QUIET_MS, 500 ms).
const RB_FREE_PROMPT_MS: u64 = 300;
/// The least a free whose repeat met a stall waits for the next repeat: the
/// pass stands down for the window after its own stall (OWED_QUIET_MS),
/// less the leg's clock slack.
const RB_YIELD_FLOOR_MS: u64 = 400;
/// Since the creates of an attempt, longer than any window the pass may stand
/// down for (OWED_QUIET_MS and a margin): every stall begun before a create
/// that went through had revived by then.
const RB_QUIET_MS: u64 = 600;
/// The owed destroy runs at the first fence pump after the engine revives,
/// itself the broker's next pump after the stalled create retired (ms).
const RB_CREATE_RECOVER_MS: u64 = 2000;

/// The seam's owed-destroy census on the warp ctl: the gauges, and the monotone
/// counters, which a recovery inside one serve pass cannot hide. The lever's
/// shot lets its chain retire before the broker reports the deadline missed,
/// so the reply that reports the stall already reads the engine live, and the
/// owed destroy may run before this program reads anything; a real stall's
/// reply reads it stalled until the chain retires.
#[derive(Clone, Copy)]
struct Owed {
    owed: u64,
    condemned: u64,
    stalls: u64,
    incurred: u64,
    ran: u64,
    failed: u64,
}

fn owed_census(w: i64) -> Owed {
    let s = open_read_all(w, "ctl");
    let f = |k: &str| parse_field(&s, k).unwrap_or_else(|| rb_incomplete("instrument:owed-census"));
    Owed {
        owed: f("ctx-owed"),
        condemned: f("slots-condemned"),
        stalls: f("gpu-stalls"),
        incurred: f("owed-incurred"),
        ran: f("owed-ran"),
        failed: f("destroy-failed"),
    }
}

/// Wait, within RB_CREATE_RECOVER_MS, for the debt incurred since `b` to be
/// paid: one owed destroy run, the gauges back where they were.
fn owed_settle(w: i64, b: Owed) -> (Owed, u64) {
    let t = libthyla_rs::time::Instant::now();
    let mut n = owed_census(w);
    while !(n.ran == b.ran + 1 && n.owed == b.owed && n.condemned == b.condemned)
        && (t.elapsed().as_millis() as u64) < RB_CREATE_RECOVER_MS
    {
        let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(50));
        n = owed_census(w);
    }
    (n, t.elapsed().as_millis() as u64)
}

/// Mint on `w` (the lowest free slot, so the one just recovered, under the id
/// the broker held) and destroy what it made.
fn owed_remint(w: i64) -> bool {
    let again = try_open_read(w, "ctx/new").and_then(|s| parse_u32_prefix(&s));
    if let Some(id) = again {
        let _ = write_ctl(w, &format!("ctx/{}/ctl", id), "destroy");
    }
    again.is_some()
}

/// The verdict line both arms print once constructed.
fn owed_verdict(arm: &str, what: &str, b: Owed, n: Owed, ms: u64, again: bool) -> bool {
    let ok = n.incurred == b.incurred + 1 && n.ran == b.ran + 1 && n.owed == b.owed && n.condemned == b.condemned && again;
    t_putstr(&format!(
        "warp-prove: C6-RB {} {} -- {} met the stall shot; destroys owed +{} (want +1, not condemned for good), owed destroys run +{} (want +1) within {} ms; ctx-owed {} slots-condemned {} after (want {} and {}); a mint of the slot's id then {}\n",
        arm,
        if ok { "PASS" } else { "FAIL" },
        what,
        n.incurred.wrapping_sub(b.incurred),
        n.ran.wrapping_sub(b.ran),
        ms,
        n.owed,
        n.condemned,
        b.owed,
        b.condemned,
        if again { "succeeded" } else { "FAILED" }
    ));
    ok
}

/// A stall meeting a context create used to leave the broker holding the id
/// while the seam re-picked the slot for every later mint, failing them all.
/// Its own connection: the lever aims at the caller's own next create.
fn rb_stalled_create() -> bool {
    for k in 1..=RB_STALL_ATTEMPTS {
        if let Some(ok) = rb_create_attempt(k) {
            return ok;
        }
    }
    t_putstr(&format!(
        "warp-prove: C6-RB CREATE UNCONSTRUCTED -- in {} attempts the stall shot never met the create\n",
        RB_STALL_ATTEMPTS
    ));
    false
}

/// One attempt: Some(verdict), or None when the shot did not meet the create
/// (another command took it, or a standing stall refused the create unstaged).
fn rb_create_attempt(k: u32) -> Option<bool> {
    let w = warp_connect("create");
    let b = owed_census(w);
    if !write_ctl(w, "ctl", "warp-stall-create") {
        rb_incomplete("instrument:stall-create-refused");
    }
    let first = try_open_read(w, "ctx/new").and_then(|s| parse_u32_prefix(&s));
    let a = owed_census(w);
    if first.is_some() || a.stalls == b.stalls {
        if let Some(id) = first {
            let _ = write_ctl(w, &format!("ctx/{}/ctl", id), "destroy");
        }
        unsafe { t_close(w) };
        t_putstr(&format!(
            "warp-prove: C6-RB CREATE attempt {} not constructed -- ctx/new {} and the engine's stalls moved by {}\n",
            k,
            if first.is_some() { "succeeded" } else { "failed" },
            a.stalls.wrapping_sub(b.stalls)
        ));
        return None;
    }
    let (n, ms) = owed_settle(w, b);
    let again = owed_remint(w);
    unsafe { t_close(w) };
    Some(owed_verdict("CREATE", "ctx/new", b, n, ms, again))
}

/// A stall meeting a ctx destroy used to leave the broker's ledger retiring
/// the id for good and the seam condemning its slot for good: eight of them
/// and no client could mint.
fn rb_stalled_destroy() -> bool {
    for k in 1..=RB_STALL_ATTEMPTS {
        if let Some(ok) = rb_destroy_attempt(k) {
            return ok;
        }
    }
    t_putstr(&format!(
        "warp-prove: C6-RB DESTROY UNCONSTRUCTED -- in {} attempts the stall shot never met the destroy\n",
        RB_STALL_ATTEMPTS
    ));
    false
}

/// One attempt: Some(verdict), or None when the shot did not meet the destroy.
/// The destroy not completing is the construction, owed or not: a build that
/// condemns it for good is what this tells apart.
fn rb_destroy_attempt(k: u32) -> Option<bool> {
    let w = warp_connect("destroy");
    let ctx = match try_open_read(w, "ctx/new").and_then(|s| parse_u32_prefix(&s)) {
        Some(c) => c,
        None => rb_incomplete("instrument:destroy-mint"),
    };
    let b = owed_census(w);
    if !write_ctl(w, "ctl", "warp-stall-destroy") {
        rb_incomplete("instrument:stall-destroy-refused");
    }
    let destroyed = write_ctl(w, &format!("ctx/{}/ctl", ctx), "destroy");
    let a = owed_census(w);
    if !destroyed || a.stalls == b.stalls || a.failed == b.failed {
        unsafe { t_close(w) };
        t_putstr(&format!(
            "warp-prove: C6-RB DESTROY attempt {} not constructed -- the destroy write {}, the engine's stalls moved by {}, destroys not completed +{}\n",
            k,
            if destroyed { "was taken" } else { "was refused" },
            a.stalls.wrapping_sub(b.stalls),
            a.failed.wrapping_sub(b.failed)
        ));
        return None;
    }
    let (n, ms) = owed_settle(w, b);
    let again = owed_remint(w);
    unsafe { t_close(w) };
    Some(owed_verdict("DESTROY", "the ctx's destroy", b, n, ms, again))
}

/// The broker's owed frees on the warp ctl: the gauge, the monotone counts,
/// the pass's stand-downs, the frees it tried first as a parked ctx's turn,
/// and the engine's stalls as the seam last saw them.
#[derive(Clone, Copy)]
struct Frees {
    owed: u64,
    incurred: u64,
    paid: u64,
    leaked: u64,
    retiring: u64,
    stood_down: u64,
    parked_first: u64,
    stalls: u64,
}

fn frees_census(w: i64) -> Frees {
    let s = open_read_all(w, "ctl");
    let f = |k: &str| parse_field(&s, k).unwrap_or_else(|| rb_incomplete("instrument:frees-census"));
    Frees {
        owed: f("frees-owed"),
        incurred: f("frees-incurred"),
        paid: f("frees-paid"),
        leaked: f("frees-leaked"),
        retiring: f("frees-retiring"),
        stood_down: f("frees-stood-down"),
        parked_first: f("frees-parked-first"),
        stalls: f("gpu-stalls"),
    }
}

/// A stall meeting a resource free used to leave the broker's ledger retiring
/// the id for good, its pins held: enough of them and no client could create.
/// Its own connection: the lever aims at the caller's next BO free.
fn rb_stalled_free() -> bool {
    for k in 1..=RB_STALL_ATTEMPTS {
        if let Some(ok) = rb_free_attempt(k) {
            return ok;
        }
    }
    t_putstr(&format!(
        "warp-prove: C6-RB FREE UNCONSTRUCTED -- in {} attempts the stall shot never met the free\n",
        RB_STALL_ATTEMPTS
    ));
    false
}

/// One attempt: Some(verdict), or None when the shot did not meet the free.
/// The broker pays an owed free at its next pass, so the counts are waited on.
fn rb_free_attempt(k: u32) -> Option<bool> {
    let w = warp_connect("free");
    let ctx = mint_ctx(w, "free");
    let (bo, _res, _va) = mint_bo_wh(w, ctx, RB_W, RB_H, VIRGL_RESOURCE_Y_0_TOP, "free");
    let b = frees_settled(w).unwrap_or_else(|_| rb_incomplete("instrument:frees-owed-at-baseline"));
    if !write_ctl(w, "ctl", "warp-stall-free") {
        rb_incomplete("instrument:stall-free-refused");
    }
    let destroyed = write_ctl(w, &format!("ctx/{}/bo/{}/ctl", ctx, bo), "destroy");
    let a = frees_census(w);
    if !destroyed || a.stalls == b.stalls {
        let _ = write_ctl(w, &format!("ctx/{}/ctl", ctx), "destroy");
        unsafe { t_close(w) };
        t_putstr(&format!(
            "warp-prove: C6-RB FREE attempt {} not constructed -- the BO destroy {}, the engine's stalls moved by {}\n",
            k,
            if destroyed { "was taken" } else { "was refused" },
            a.stalls.wrapping_sub(b.stalls)
        ));
        return None;
    }
    let t = libthyla_rs::time::Instant::now();
    let mut n = a;
    while !(n.paid == b.paid + 1 && n.owed == b.owed && n.retiring == b.retiring)
        && (t.elapsed().as_millis() as u64) < RB_CREATE_RECOVER_MS
    {
        let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(50));
        n = frees_census(w);
    }
    let ms = t.elapsed().as_millis() as u64;
    let _ = write_ctl(w, &format!("ctx/{}/ctl", ctx), "destroy");
    unsafe { t_close(w) };
    // A stall past the destroy's (the repeat itself missing its deadline)
    // stands the pass down, so the repay comes a window later: not this one.
    if n.stalls != a.stalls {
        t_putstr(&format!(
            "warp-prove: C6-RB FREE attempt {} not constructed -- the engine stalled {} more times before the repay\n",
            k,
            n.stalls.wrapping_sub(a.stalls)
        ));
        return None;
    }
    let ok = n.incurred == b.incurred + 1
        && n.paid == b.paid + 1
        && n.owed == b.owed
        && n.leaked == b.leaked
        && n.retiring == b.retiring
        && ms < RB_FREE_PROMPT_MS;
    t_putstr(&format!(
        "warp-prove: C6-RB FREE {} -- the BO's free met the stall shot; frees owed +{} (want +1), paid by a repeat +{} (want +1) within {} ms (want < {}: the stall met the client's free, not the pass, so the pass did not stand down), given up +{} (want 0); frees-owed {} after (want {}); frees-retiring {} after (want {}, the id retired and its pins gone)\n",
        if ok { "PASS" } else { "FAIL" },
        n.incurred.wrapping_sub(b.incurred),
        n.paid.wrapping_sub(b.paid),
        ms,
        RB_FREE_PROMPT_MS,
        n.leaked.wrapping_sub(b.leaked),
        n.owed,
        b.owed,
        n.retiring,
        b.retiring
    ));
    Some(ok)
}

/// The census once no free is owed, so an earlier arm's debt paid inside a
/// window cannot read as the window's own; Err with the census if one stays.
fn frees_settled(w: i64) -> Result<Frees, Frees> {
    let t0 = libthyla_rs::time::Instant::now();
    let mut b = frees_census(w);
    while b.owed != 0 && (t0.elapsed().as_millis() as u64) < RB_CREATE_RECOVER_MS {
        let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(50));
        b = frees_census(w);
    }
    if b.owed != 0 { Err(b) } else { Ok(b) }
}

/// A BO free the broker refused while a poisoned chain could reach the BO
/// was never asked again: the ledger entry and the pins stayed for the boot,
/// while the vindication uncharged the ctx. The broker now takes such a free
/// as a debt and pays it once the chain has retired, links first.
fn rb_taken_free() -> bool {
    for k in 1..=RB_STALL_ATTEMPTS {
        if let Some(ok) = rb_taken_attempt(k) {
            return ok;
        }
    }
    t_putstr(&format!(
        "warp-prove: C6-RB TAKEN UNCONSTRUCTED -- in {} attempts the BO's free never met a poisoned ctx with no stall\n",
        RB_STALL_ATTEMPTS
    ));
    false
}

/// How long a debt must stay unpaid while its chain is poisoned: many serve
/// passes, each of which would pay a free nothing held.
const RB_TAKEN_HELD_MS: u64 = 300;

/// One attempt: Some(verdict), or None when the ctx did not poison or a stall
/// met the destroy (a stall's debt would read as a taken one).
fn rb_taken_attempt(k: u32) -> Option<bool> {
    let w = warp_connect("taken");
    let ctx = mint_ctx(w, "taken");
    let (bo, res, _va) = mint_bo_wh(w, ctx, RB_W, RB_H, VIRGL_RESOURCE_Y_0_TOP, "taken");
    // Past FREE, a debt still owed is one the broker never paid: a failure of
    // the mechanism under test, not of the instrument.
    let b = match frees_settled(w) {
        Ok(b) => b,
        Err(b) => {
            let _ = write_ctl(w, &format!("ctx/{}/ctl", ctx), "destroy");
            unsafe { t_close(w) };
            t_putstr(&format!(
                "warp-prove: C6-RB TAKEN FAIL -- a free owed before the window was never paid (frees-owed {}, paid {}, given up {})\n",
                b.owed, b.paid, b.leaked
            ));
            return Some(false);
        }
    };
    // One chain on the ctx, kept from completing, then abandoned: the ctx is
    // poisoned, and its chain reaches every resource linked to it.
    if !write_ctl(w, "ctl", "warp-hold on") {
        rb_incomplete("instrument:taken-hold");
    }
    let mut st: Vec<u32> = Vec::new();
    subctx_preamble(&mut st);
    clear_stream(&mut st, res, 1, 0.0, 1.0, 0.0);
    submit_stream(w, ctx, &st, "taken");
    if !write_ctl(w, "ctl", "warp-abandon") {
        rb_incomplete("instrument:taken-abandon");
    }
    // A destroy on a ctx with fences in flight is deferred, not sent.
    let t = libthyla_rs::time::Instant::now();
    let mut left = ctx_field(w, ctx, "fences-in-flight");
    while left != 0 && t.elapsed().as_millis() < 2000 {
        let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(50));
        left = ctx_field(w, ctx, "fences-in-flight");
    }
    let ctl = format!("ctx/{}/ctl", ctx);
    let poisoned = parse_field(&open_read_string(w, &ctl), "poisoned").unwrap_or(99);
    let p = frees_census(w);
    let destroyed = poisoned == 1 && left == 0 && write_ctl(w, &format!("ctx/{}/bo/{}/ctl", ctx, bo), "destroy");
    let a = frees_census(w);
    let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(RB_TAKEN_HELD_MS));
    let held = frees_census(w);
    let _ = write_ctl(w, "ctl", "warp-hold off");
    if !destroyed || a.stalls != p.stalls || held.stalls != p.stalls {
        let _ = write_ctl(w, &ctl, "destroy");
        unsafe { t_close(w) };
        t_putstr(&format!(
            "warp-prove: C6-RB TAKEN attempt {} not constructed -- ctx poisoned {} (want 1), fences in flight {} (want 0), the BO destroy {}, the engine's stalls moved by {}\n",
            k,
            poisoned,
            left,
            if destroyed { "was taken" } else { "was not sent or was refused" },
            held.stalls.wrapping_sub(p.stalls)
        ));
        return None;
    }
    let t = libthyla_rs::time::Instant::now();
    let mut n = held;
    while !(n.paid == b.paid + 1 && n.owed == b.owed && n.retiring == b.retiring)
        && (t.elapsed().as_millis() as u64) < RB_CREATE_RECOVER_MS
    {
        let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(50));
        n = frees_census(w);
    }
    let ms = t.elapsed().as_millis() as u64;
    let _ = write_ctl(w, &ctl, "destroy");
    unsafe { t_close(w) };
    let taken = a.incurred == b.incurred + 1 && a.owed == b.owed + 1 && a.retiring == b.retiring + 1;
    let waited = held.paid == b.paid && held.owed == b.owed + 1;
    let paid = n.paid == b.paid + 1 && n.owed == b.owed && n.leaked == b.leaked && n.retiring == b.retiring;
    let ok = taken && waited && paid;
    t_putstr(&format!(
        "warp-prove: C6-RB TAKEN {} -- the BO's free met its poisoned ctx; taken: frees owed +{} incurred +{} retiring +{} (want +1 each); {} ms on, still poisoned: paid +{} (want 0) owed +{} (want +1); after the vindication: paid +{} (want +1) within {} ms, owed {} (want {}), given up +{} (want 0), retiring {} (want {}, the links detached, the id retired, the pins gone)\n",
        if ok { "PASS" } else { "FAIL" },
        a.owed.wrapping_sub(b.owed),
        a.incurred.wrapping_sub(b.incurred),
        a.retiring.wrapping_sub(b.retiring),
        RB_TAKEN_HELD_MS,
        held.paid.wrapping_sub(b.paid),
        held.owed.wrapping_sub(b.owed),
        n.paid.wrapping_sub(b.paid),
        ms,
        n.owed,
        b.owed,
        n.leaked.wrapping_sub(b.leaked),
        n.retiring,
        b.retiring
    ));
    Some(ok)
}

/// A taken free waited for every chain on the device that could reach it,
/// and after the vindication its ctx is live again: a ctx kept busy held the
/// free off for as long as it rendered, while the seam had uncharged it. A
/// chain that can reach an owed free now parks until the free is paid.
fn rb_taken_busy() -> bool {
    rb_busy_arm(Busy::Plain)
}

/// The barrier stood down for every owed free once the pass met a stall at
/// any one, so the pass's repeat of another client's free let a busy ctx's
/// chains past its own. It stands down only for the free whose own repeat
/// met the stall.
fn rb_apart() -> bool {
    rb_busy_arm(Busy::Apart)
}

/// The barrier's half of a free's stand-down: once the free's own repeat
/// meets a stall, the ctx holding it goes to the device for the window, even
/// while it holds another owed free, and the frees wait for its chain as for
/// any; then the barrier stands again. A ctx with a parked chain has its
/// free tried as its turn, whatever else is owed.
fn rb_lift() -> bool {
    rb_busy_arm(Busy::Lift)
}

/// BUSY's attempt, and what APART and LIFT do once the vindicated ctx's next
/// chain is parked behind the deferred free.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Busy {
    Plain,
    /// Another client's free stands the pass down; the chain must stay parked.
    Apart,
    /// The first of the ctx's two frees stands down on its own repeat; the
    /// chain must go to the device, and the next one park after the window.
    Lift,
}

impl Busy {
    fn name(self) -> &'static str {
        match self {
            Busy::Plain => "BUSY",
            Busy::Apart => "APART",
            Busy::Lift => "LIFT",
        }
    }
}

fn rb_busy_arm(mode: Busy) -> bool {
    for k in 1..=RB_STALL_ATTEMPTS {
        if let Some(ok) = rb_busy_attempt(k, mode) {
            return ok;
        }
    }
    t_putstr(&format!(
        "warp-prove: C6-RB {} UNCONSTRUCTED -- in {} attempts {}\n",
        mode.name(),
        RB_STALL_ATTEMPTS,
        match mode {
            Busy::Plain => "the taken free was never still owed when the live ctx's next chain went in",
            Busy::Apart => "another client's free and its repeat never met their shots while the chain waited behind the deferred free",
            Busy::Lift => "the free's own repeat never met its shot while the chain waited behind it",
        }
    ));
    false
}

/// How long a broker may take to pay a free nothing on the device can reach:
/// a few serve passes. A chain kept on the device holds it far longer.
const RB_BUSY_PAY_MS: u64 = 1000;
/// APART: how soon after the defer's second arm the other client's free is
/// paid, a window after its repeat's stall; and the latest the chain may be
/// looked at, inside that arm (Lictor's TEST_OWED_DEFER_MS, 2 s) by a margin.
const RB_APART_DUE_MS: u64 = 1500;
const RB_APART_DEFER_MS: u64 = 1800;

/// The ctx's fenced chains on the warp ctl's slot rows: (parked, on the
/// device). The ctx rows follow the slot rows, so the ctx's own row proves
/// the read reached them.
fn ctx_chains(w: i64, ctx: u32) -> (u32, u32) {
    let s = open_read_all(w, "ctl");
    let want = format!("ctx={}", ctx);
    let own = format!("wctx {} ", ctx);
    if !s.lines().any(|l| l.starts_with(&own)) {
        rb_incomplete("instrument:ctl-rows-cut");
    }
    let (mut parked, mut on_device) = (0, 0);
    for line in s.lines() {
        let mut it = line.split_ascii_whitespace();
        let kind = it.next();
        if !it.any(|t| t == want) {
            continue;
        }
        match kind {
            Some("fenced-parked") => parked += 1,
            Some("fenced-held") => on_device += 1,
            _ => {}
        }
    }
    (parked, on_device)
}

/// Lictor keeps one owed-stall shot at a time, and drops one no repeat took
/// within TEST_OWED_DEFER_MS (2 s): an earlier attempt's is waited out.
const RB_REPAY_ARM_MS: u64 = 2500;

fn arm_stall_repay(w: i64, bo_ctl: &str) {
    let t = libthyla_rs::time::Instant::now();
    while !write_ctl(w, bo_ctl, "stall-repay") {
        if (t.elapsed().as_millis() as u64) >= RB_REPAY_ARM_MS {
            rb_incomplete("instrument:stall-repay-refused");
        }
        let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(50));
    }
}

/// Destroy an attempt's ctxs and close their connections.
fn busy_quit(w: i64, ctx: u32, other: Option<(i64, u32, u32)>) {
    let _ = write_ctl(w, &format!("ctx/{}/ctl", ctx), "destroy");
    unsafe { t_close(w) };
    if let Some((w2, ctx2, _)) = other {
        let _ = write_ctl(w2, &format!("ctx/{}/ctl", ctx2), "destroy");
        unsafe { t_close(w2) };
    }
}

/// One attempt: Some(verdict), or None when it was not constructed (the ctx
/// did not poison, a stall met a step, the free was paid before the live
/// ctx's next chain went in, or a shot or the defer ran out first).
fn rb_busy_attempt(k: u32, mode: Busy) -> Option<bool> {
    let arm = mode.name();
    let w = warp_connect("busy");
    let ctx = mint_ctx(w, "busy");
    let (bo, res, _va) = mint_bo_wh(w, ctx, RB_W, RB_H, VIRGL_RESOURCE_Y_0_TOP, "busy");
    let (_bo2, res2, _va2) = mint_bo_wh(w, ctx, RB_W, RB_H, VIRGL_RESOURCE_Y_0_TOP, "busy next");
    // LIFT's second free on the ctx: one free's stand-down must let the ctx
    // go though it holds the other.
    let bo3 = (mode == Busy::Lift).then(|| mint_bo_wh(w, ctx, RB_W, RB_H, VIRGL_RESOURCE_Y_0_TOP, "lift second").0);
    let frees: u64 = if bo3.is_some() { 2 } else { 1 };
    let ctl = format!("ctx/{}/ctl", ctx);
    let bo_ctl = format!("ctx/{}/bo/{}/ctl", ctx, bo);
    // APART's other client, on its own connection, minted with the rest.
    let other = (mode == Busy::Apart).then(|| {
        let w2 = warp_connect("apart");
        let ctx2 = mint_ctx(w2, "apart");
        let (bo_o, _res_o, _va_o) = mint_bo_wh(w2, ctx2, RB_W, RB_H, VIRGL_RESOURCE_Y_0_TOP, "apart");
        (w2, ctx2, bo_o)
    });
    // The creates went through, so every stall begun before them had revived:
    // with no stall begun since (`m`), no window the pass stands down for,
    // nor any barrier, outlasts RB_QUIET_MS from here.
    let m = frees_census(w);
    let minted = libthyla_rs::time::Instant::now();
    let b = match frees_settled(w) {
        Ok(b) => b,
        Err(b) => {
            busy_quit(w, ctx, other);
            t_putstr(&format!(
                "warp-prove: C6-RB {} FAIL -- a free owed before the window was never paid (frees-owed {}, paid {}, given up {})\n",
                arm, b.owed, b.paid, b.leaked
            ));
            return Some(false);
        }
    };
    // As TAKEN: the ctx poisoned, then the BO's free taken.
    if !write_ctl(w, "ctl", "warp-hold on") {
        rb_incomplete("instrument:busy-hold");
    }
    let mut st: Vec<u32> = Vec::new();
    subctx_preamble(&mut st);
    clear_stream(&mut st, res, 1, 0.0, 1.0, 0.0);
    submit_stream(w, ctx, &st, "busy");
    if !write_ctl(w, "ctl", "warp-abandon") {
        rb_incomplete("instrument:busy-abandon");
    }
    let t = libthyla_rs::time::Instant::now();
    let mut left = ctx_field(w, ctx, "fences-in-flight");
    while left != 0 && t.elapsed().as_millis() < 2000 {
        let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(50));
        left = ctx_field(w, ctx, "fences-in-flight");
    }
    let poisoned = |w: i64| parse_field(&open_read_string(w, &ctl), "poisoned").unwrap_or(99);
    let poisoned_at_destroy = poisoned(w);
    // Once no window can still be open, so every barrier stands when the
    // ctx's next chain goes in.
    while (minted.elapsed().as_millis() as u64) < RB_QUIET_MS {
        let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(20));
    }
    // LIFT's shot rides the pass's first repeat of this BO's free: armed while
    // the BO lives, taken once the defer is lifted. This free is owed first,
    // so the pass comes to it first.
    if mode == Busy::Lift {
        arm_stall_repay(w, &bo_ctl);
    }
    let destroyed = poisoned_at_destroy == 1
        && left == 0
        && write_ctl(w, &bo_ctl, "destroy")
        && match bo3 {
            Some(bo3) => write_ctl(w, &format!("ctx/{}/bo/{}/ctl", ctx, bo3), "destroy"),
            None => true,
        };
    let a = frees_census(w);
    let taken = destroyed && a.incurred == b.incurred + frees && a.owed == b.owed + frees && a.retiring == b.retiring + frees;
    // The broker passes over this ctx's owed frees for a while, so the free
    // is still owed when the ctx, vindicated, sends its next chain; held, a
    // chain that goes to the device stays there.
    let deferred = taken && write_ctl(w, "ctl", "warp-defer-frees on");
    let _ = write_ctl(w, "ctl", "warp-hold off");
    let t = libthyla_rs::time::Instant::now();
    let mut live = poisoned(w);
    while live != 0 && t.elapsed().as_millis() < 2000 {
        let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(20));
        live = poisoned(w);
    }
    let held = deferred && live == 0 && write_ctl(w, "ctl", "warp-hold on");
    if held {
        let mut st: Vec<u32> = Vec::new();
        subctx_preamble(&mut st);
        clear_stream(&mut st, res2, 1, 0.0, 0.0, 1.0);
        submit_stream(w, ctx, &st, "busy next");
    }
    // APART's other free needs the defer longer than its first arm may have
    // left: armed again, its clock read before the write.
    let rearm_t = libthyla_rs::time::Instant::now();
    let rearmed = mode != Busy::Apart || (held && write_ctl(w, "ctl", "warp-defer-frees on"));
    let s = frees_census(w);
    let (parked_s, on_device_s) = if held { ctx_chains(w, ctx) } else { (0, 0) };
    let undeferred = deferred && mode != Busy::Apart && write_ctl(w, "ctl", "warp-defer-frees off");
    if !(held && rearmed && s.owed == b.owed + frees && s.paid == b.paid && s.stalls == m.stalls) {
        if mode == Busy::Apart && deferred {
            let _ = write_ctl(w, "ctl", "warp-defer-frees off");
        }
        let _ = write_ctl(w, "ctl", "warp-hold off");
        busy_quit(w, ctx, other);
        t_putstr(&format!(
            "warp-prove: C6-RB {} attempt {} not constructed -- ctx poisoned {} at the destroy (want 1), fences in flight {} (want 0), the free {}, the defer {}, poisoned {} after the vindication (want 0), at the next chain frees owed +{} (want +{}) paid +{} (want 0), the engine's stalls moved by {}\n",
            arm,
            k,
            poisoned_at_destroy,
            left,
            if taken { "taken" } else { "not taken" },
            if deferred && rearmed { "armed" } else { "refused" },
            live,
            s.owed.wrapping_sub(b.owed),
            frees,
            s.paid.wrapping_sub(b.paid),
            s.stalls.wrapping_sub(m.stalls)
        ));
        return None;
    }
    // APART and LIFT need the chain parked behind the free before their
    // stall; BUSY reads it from the payment instead.
    if mode != Busy::Plain && !(parked_s >= 1 && on_device_s == 0) {
        if mode == Busy::Apart {
            let _ = write_ctl(w, "ctl", "warp-defer-frees off");
        }
        let _ = write_ctl(w, "ctl", "warp-hold off");
        busy_quit(w, ctx, other);
        t_putstr(&format!(
            "warp-prove: C6-RB {} FAIL -- the vindicated ctx's next chain went to the device past the owed free: parked {}, on the device {} (want 1 and 0)\n",
            arm, parked_s, on_device_s
        ));
        return Some(false);
    }
    if mode == Busy::Lift {
        // Once the defer is lifted the pass repeats the first free, as the
        // parked ctx's turn, and that repeat meets the shot: the free's
        // barrier stands down, and the ctx, though it holds the second free
        // too, goes to the device for the window.
        let t = libthyla_rs::time::Instant::now();
        let mut n = frees_census(w);
        while !(n.stood_down != s.stood_down && n.stalls != s.stalls)
            && n.paid == s.paid
            && (t.elapsed().as_millis() as u64) < RB_BUSY_PAY_MS
        {
            let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(20));
            n = frees_census(w);
        }
        let stood = n.stood_down == s.stood_down + 1;
        // A payment first, more than the one stall, or a stall the pass did
        // not meet: not this window.
        if n.stalls > s.stalls + 1 || n.paid != s.paid || (n.stalls != s.stalls && !stood) {
            let _ = write_ctl(w, "ctl", "warp-hold off");
            busy_quit(w, ctx, None);
            t_putstr(&format!(
                "warp-prove: C6-RB LIFT attempt {} not constructed -- with the defer lifted the engine stalled {} times (want 1), the pass stood down +{} (want +1), paid +{} first (want 0)\n",
                k,
                n.stalls.wrapping_sub(s.stalls),
                n.stood_down.wrapping_sub(s.stood_down),
                n.paid.wrapping_sub(s.paid)
            ));
            return None;
        }
        if !stood {
            let (parked, on_device) = ctx_chains(w, ctx);
            let _ = write_ctl(w, "ctl", "warp-hold off");
            busy_quit(w, ctx, None);
            t_putstr(&format!(
                "warp-prove: C6-RB LIFT FAIL -- with the defer lifted the free's repeat never met its shot within {} ms (no stall, paid +0); the chain parked {} on the device {}: {}\n",
                RB_BUSY_PAY_MS,
                parked,
                on_device,
                if on_device > 0 { "it went to the device with no stand-down and held the frees off" } else { "the pass never repeated the free" }
            ));
            return Some(false);
        }
        if n.parked_first != s.parked_first + 1 {
            let _ = write_ctl(w, "ctl", "warp-hold off");
            busy_quit(w, ctx, None);
            t_putstr(&format!(
                "warp-prove: C6-RB LIFT FAIL -- the repeat that met the shot was not tried as the parked ctx's turn (parked-first +{}, want +1)\n",
                n.parked_first.wrapping_sub(s.parked_first)
            ));
            return Some(false);
        }
        let seen = libthyla_rs::time::Instant::now();
        let mut went = ctx_chains(w, ctx);
        while went.1 == 0 && (seen.elapsed().as_millis() as u64) < RB_QUIET_MS {
            let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(20));
            went = ctx_chains(w, ctx);
        }
        let went_ms = seen.elapsed().as_millis() as u64;
        // Past the window the chain on the device still holds both frees off,
        // and the barrier stands again: the ctx's next chain parks.
        while (seen.elapsed().as_millis() as u64) < RB_QUIET_MS {
            let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(20));
        }
        let c = frees_census(w);
        let mut st: Vec<u32> = Vec::new();
        subctx_preamble(&mut st);
        clear_stream(&mut st, res2, 1, 1.0, 0.0, 0.0);
        submit_stream(w, ctx, &st, "lift after");
        let (after_parked, after_on_device) = ctx_chains(w, ctx);
        let _ = write_ctl(w, "ctl", "warp-hold off");
        let t = libthyla_rs::time::Instant::now();
        let mut e = frees_census(w);
        while !(e.paid == b.paid + frees && e.owed == b.owed && e.retiring == b.retiring)
            && (t.elapsed().as_millis() as u64) < RB_BUSY_PAY_MS
        {
            let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(20));
            e = frees_census(w);
        }
        let ms = t.elapsed().as_millis() as u64;
        // A stall after the shot while the frees were due may stand one down
        // again past the bound: not this window.
        if e.stalls != s.stalls + 1 {
            busy_quit(w, ctx, None);
            t_putstr(&format!(
                "warp-prove: C6-RB LIFT attempt {} not constructed -- the engine stalled {} more times after the shot while the frees were due\n",
                k,
                e.stalls.wrapping_sub(s.stalls + 1)
            ));
            return None;
        }
        let t = libthyla_rs::time::Instant::now();
        let mut next_left = ctx_field(w, ctx, "fences-in-flight");
        while next_left != 0 && t.elapsed().as_millis() < 2000 {
            let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(20));
            next_left = ctx_field(w, ctx, "fences-in-flight");
        }
        busy_quit(w, ctx, None);
        let ok = went.1 >= 1
            && c.paid == s.paid
            && c.owed == s.owed
            && after_parked >= 1
            && e.paid == b.paid + frees
            && e.owed == b.owed
            && e.leaked == b.leaked
            && e.retiring == b.retiring
            && next_left == 0;
        t_putstr(&format!(
            "warp-prove: C6-RB LIFT {} -- the first of the ctx's two frees met its shot on its own repeat once the defer was {} (the pass stood down +1, the parked ctx's turn): the chain {} the device {} ms after (want within {}); {} ms after the stand-down, the chain held, paid +{} (want 0: the chain on the device holds the frees) owed {} (want {}); the ctx's next chain then parked {} on the device {} (want parked: the window had closed); after the hold paid +{} (want +2) within {} ms (bound {}), owed {} (want {}), given up +{} (want 0), retiring {} (want {}), fences in flight {} (want 0): {}\n",
            if ok { "PASS" } else { "FAIL" },
            if undeferred { "lifted" } else { "gone" },
            if went.1 >= 1 { "reached" } else { "never reached" },
            went_ms,
            RB_QUIET_MS,
            RB_QUIET_MS,
            c.paid.wrapping_sub(s.paid),
            c.owed,
            s.owed,
            after_parked,
            after_on_device,
            e.paid.wrapping_sub(b.paid),
            ms,
            RB_BUSY_PAY_MS,
            e.owed,
            b.owed,
            e.leaked.wrapping_sub(b.leaked),
            e.retiring,
            b.retiring,
            next_left,
            if ok {
                "the free's own stand-down let the ctx go though it held the other, the window closed, and the frees waited for its chain"
            } else if went.1 == 0 {
                "the chain stayed parked through the free's own stand-down"
            } else if c.paid != s.paid {
                "the chain went only once the frees were paid"
            } else if after_parked == 0 {
                "the window never closed: the ctx's next chain went to the device past the frees"
            } else if next_left != 0 {
                "the chains never ran"
            } else {
                "the frees were not paid after the hold"
            }
        ));
        return Some(ok);
    }
    // APART: another client's free, and the pass's repeat of it, each meet a
    // stall shot while this ctx's chain waits behind its deferred free.
    let mut apart: Option<(u64, Frees, u32, u32)> = None;
    let mut due_from = s;
    if let Some((w2, ctx2, bo_o)) = other {
        let o_ctl = format!("ctx/{}/bo/{}/ctl", ctx2, bo_o);
        arm_stall_repay(w2, &o_ctl);
        if !write_ctl(w2, "ctl", "warp-stall-free") {
            rb_incomplete("instrument:stall-free-refused");
        }
        let destroyed_o = write_ctl(w2, &o_ctl, "destroy");
        let a_o = frees_census(w);
        // The pass repeats the other free at its next turn, that repeat meets
        // the second shot, and the other free is paid a window later.
        let mut n = a_o;
        while n.paid == s.paid
            && n.stalls <= s.stalls + 2
            && (rearm_t.elapsed().as_millis() as u64) < RB_APART_DUE_MS
        {
            let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(20));
            n = frees_census(w);
        }
        let (parked, on_device) = ctx_chains(w, ctx);
        let looked_ms = rearm_t.elapsed().as_millis() as u64;
        let stood = n.stood_down == s.stood_down + 1;
        if !destroyed_o
            || a_o.stalls == s.stalls
            || n.stalls > s.stalls + 2
            || (n.stalls == s.stalls + 2 && !stood)
            || (n.paid != s.paid && !stood)
            || looked_ms >= RB_APART_DEFER_MS
        {
            let _ = write_ctl(w, "ctl", "warp-defer-frees off");
            let _ = write_ctl(w, "ctl", "warp-hold off");
            busy_quit(w, ctx, other);
            t_putstr(&format!(
                "warp-prove: C6-RB APART attempt {} not constructed -- the other client's BO destroy {}, the engine's stalls moved by {} (want 2), the pass stood down +{} (want +1), the chain looked at {} ms into the defer's arm (bound {})\n",
                k,
                if destroyed_o { "was taken" } else { "was refused" },
                n.stalls.wrapping_sub(s.stalls),
                n.stood_down.wrapping_sub(s.stood_down),
                looked_ms,
                RB_APART_DEFER_MS
            ));
            return None;
        }
        apart = Some((a_o.incurred.wrapping_sub(s.incurred), n, parked, on_device));
        due_from = n;
    }
    let undeferred = undeferred || (mode == Busy::Apart && write_ctl(w, "ctl", "warp-defer-frees off"));
    let paid_want = b.paid + if mode == Busy::Apart { 2 } else { 1 };
    let t = libthyla_rs::time::Instant::now();
    let mut n = frees_census(w);
    while !(n.paid == paid_want && n.owed == b.owed && n.retiring == b.retiring)
        && (t.elapsed().as_millis() as u64) < RB_BUSY_PAY_MS
    {
        let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(20));
        n = frees_census(w);
    }
    let ms = t.elapsed().as_millis() as u64;
    let _ = write_ctl(w, "ctl", "warp-hold off");
    // A stall while the free was due may have been the pass's own, which
    // stands its barrier down and lets the held chain go: not this window.
    if n.stalls != due_from.stalls {
        busy_quit(w, ctx, other);
        t_putstr(&format!(
            "warp-prove: C6-RB {} attempt {} not constructed -- the engine stalled {} times while the free was due\n",
            arm,
            k,
            n.stalls.wrapping_sub(due_from.stalls)
        ));
        return None;
    }
    // The chain that waited for the free must then go to the device and end.
    let t = libthyla_rs::time::Instant::now();
    let mut next_left = ctx_field(w, ctx, "fences-in-flight");
    while next_left != 0 && t.elapsed().as_millis() < 2000 {
        let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(20));
        next_left = ctx_field(w, ctx, "fences-in-flight");
    }
    busy_quit(w, ctx, other);
    let paid_ok = n.paid == paid_want && n.owed == b.owed && n.leaked == b.leaked && n.retiring == b.retiring;
    if let Some((owed_o, o, parked, on_device)) = apart {
        let apart_ok = owed_o == 1
            && o.stood_down == s.stood_down + 1
            && o.stalls == s.stalls + 2
            && o.paid == s.paid + 1
            && o.owed == s.owed
            && parked >= 1
            && on_device == 0;
        let ok = apart_ok && paid_ok && next_left == 0;
        t_putstr(&format!(
            "warp-prove: C6-RB APART {} -- another client's free met its shot and the pass's repeat of it the other while this ctx's next chain waited behind the deferred free: the other free owed +{} (want +1), the engine's stalls +{} (want +2), the pass stood down +{} (want +1), the other free paid +{} (want +1); after its window the chain parked {} on the device {} (want 1 and 0: that stand-down left this free's barrier up); then with the defer {}: paid +{} in all (want +2) within {} ms (bound {}), owed {} (want {}), given up +{} (want 0), retiring {} (want {}), fences in flight {} after the hold (want 0): {}\n",
            if ok { "PASS" } else { "FAIL" },
            owed_o,
            o.stalls.wrapping_sub(s.stalls),
            o.stood_down.wrapping_sub(s.stood_down),
            o.paid.wrapping_sub(s.paid),
            parked,
            on_device,
            if undeferred { "lifted" } else { "already expired" },
            n.paid.wrapping_sub(b.paid),
            ms,
            RB_BUSY_PAY_MS,
            n.owed,
            b.owed,
            n.leaked.wrapping_sub(b.leaked),
            n.retiring,
            b.retiring,
            next_left,
            if ok {
                "the chain stayed parked, then ran"
            } else if on_device > 0 {
                "the other free's stand-down let the chain go"
            } else if !apart_ok {
                "the other free was not owed, stood down for and paid"
            } else if next_left != 0 {
                "the chain never ran"
            } else {
                "this ctx's free was not paid"
            }
        ));
        return Some(ok);
    }
    let ok = paid_ok && next_left == 0;
    t_putstr(&format!(
        "warp-prove: C6-RB BUSY {} -- the taken free was still owed when the vindicated ctx's next chain went in, held; with the defer {}: paid +{} (want +1) within {} ms (bound {}), owed {} (want {}), given up +{} (want 0), retiring {} (want {}), fences in flight {} after the hold (want 0, the chain ran): {}\n",
        if ok { "PASS" } else { "FAIL" },
        if undeferred { "lifted" } else { "already expired" },
        n.paid.wrapping_sub(b.paid),
        ms,
        RB_BUSY_PAY_MS,
        n.owed,
        b.owed,
        n.leaked.wrapping_sub(b.leaked),
        n.retiring,
        b.retiring,
        next_left,
        if ok {
            "the chain parked behind the free, then ran"
        } else if next_left != 0 {
            "the chain never ran"
        } else {
            "the chain on the device held the free off"
        }
    ));
    Some(ok)
}

/// A repeat that met a stall used to be tried again at the pass's next turn,
/// ahead of the requests, at every revival; standing down after every stall
/// instead let a workload keep the pass from ever running. The pass stands
/// down for the window after a stall its own repeat met, and only then.
fn rb_yield() -> bool {
    for k in 1..=RB_STALL_ATTEMPTS {
        if let Some(ok) = rb_yield_attempt(k) {
            return ok;
        }
    }
    t_putstr(&format!(
        "warp-prove: C6-RB YIELD UNCONSTRUCTED -- in {} attempts the free and its repeat never met their shots with no other stall\n",
        RB_STALL_ATTEMPTS
    ));
    false
}

/// One attempt: Some(verdict), or None when the destroy did not meet its shot
/// or another stall met the window.
fn rb_yield_attempt(k: u32) -> Option<bool> {
    let w = warp_connect("yield");
    let ctx = mint_ctx(w, "yield");
    let (bo, _res, _va) = mint_bo_wh(w, ctx, RB_W, RB_H, VIRGL_RESOURCE_Y_0_TOP, "yield");
    let ctl = format!("ctx/{}/ctl", ctx);
    let bo_ctl = format!("ctx/{}/bo/{}/ctl", ctx, bo);
    let b = match frees_settled(w) {
        Ok(b) => b,
        Err(b) => {
            let _ = write_ctl(w, &ctl, "destroy");
            unsafe { t_close(w) };
            t_putstr(&format!(
                "warp-prove: C6-RB YIELD FAIL -- a free owed before the window was never paid (frees-owed {}, paid {}, given up {})\n",
                b.owed, b.paid, b.leaked
            ));
            return Some(false);
        }
    };
    // The repeat's shot first: the pass repeats the free at its next turn
    // after the destroy's reply.
    arm_stall_repay(w, &bo_ctl);
    if !write_ctl(w, "ctl", "warp-stall-free") {
        rb_incomplete("instrument:stall-free-refused");
    }
    let destroyed = write_ctl(w, &bo_ctl, "destroy");
    let t = libthyla_rs::time::Instant::now();
    let a = frees_census(w);
    if !destroyed || a.stalls == b.stalls {
        let _ = write_ctl(w, &ctl, "destroy");
        unsafe { t_close(w) };
        t_putstr(&format!(
            "warp-prove: C6-RB YIELD attempt {} not constructed -- the BO destroy {}, the engine's stalls moved by {}\n",
            k,
            if destroyed { "was taken" } else { "was refused" },
            a.stalls.wrapping_sub(b.stalls)
        ));
        return None;
    }
    let mut n = a;
    while !(n.paid == b.paid + 1 && n.owed == b.owed && n.retiring == b.retiring)
        && n.stalls <= b.stalls + 2
        && (t.elapsed().as_millis() as u64) < RB_CREATE_RECOVER_MS
    {
        let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(20));
        n = frees_census(w);
    }
    let ms = t.elapsed().as_millis() as u64;
    let _ = write_ctl(w, &ctl, "destroy");
    unsafe { t_close(w) };
    if n.stalls > b.stalls + 2 {
        t_putstr(&format!(
            "warp-prove: C6-RB YIELD attempt {} not constructed -- the engine stalled {} times, more than the two shots\n",
            k,
            n.stalls.wrapping_sub(b.stalls)
        ));
        return None;
    }
    if n.stalls == b.stalls + 2 && n.stood_down == b.stood_down {
        t_putstr(&format!(
            "warp-prove: C6-RB YIELD attempt {} not constructed -- the engine stalled twice and the pass never stood down: the second stall was not its repeat's\n",
            k
        ));
        return None;
    }
    let ok = n.incurred == b.incurred + 1
        && n.stalls == b.stalls + 2
        && n.stood_down == b.stood_down + 1
        && n.paid == b.paid + 1
        && n.owed == b.owed
        && n.leaked == b.leaked
        && n.retiring == b.retiring
        && ms >= RB_YIELD_FLOOR_MS;
    t_putstr(&format!(
        "warp-prove: C6-RB YIELD {} -- the BO's free met one stall shot and the pass's repeat of it the other; frees owed +{} (want +1), the engine's stalls +{} (want +2, the repeat met its shot), the pass stood down +{} (want +1), paid by a later repeat +{} (want +1) {} ms after the destroy (want >= {}: the pass stood down for the window after its own stall; bound {}), given up +{} (want 0); frees-owed {} after (want {}); frees-retiring {} after (want {})\n",
        if ok { "PASS" } else { "FAIL" },
        n.incurred.wrapping_sub(b.incurred),
        n.stalls.wrapping_sub(b.stalls),
        n.stood_down.wrapping_sub(b.stood_down),
        n.paid.wrapping_sub(b.paid),
        ms,
        RB_YIELD_FLOOR_MS,
        RB_CREATE_RECOVER_MS,
        n.leaked.wrapping_sub(b.leaked),
        n.owed,
        b.owed,
        n.retiring,
        b.retiring
    ));
    Some(ok)
}

/// One attempt: Some(verdict), or None when it was not constructed (the
/// readback was on the device when the abandon landed, or a stall met its sends).
fn rb_abandon_attempt(k: u32, tap: i64) -> Option<bool> {
    let w = warp_connect("abandon");
    let ctx = mint_ctx(w, "abandon");
    let (bo, res, _va) = mint_bo_wh(w, ctx, RB_W, RB_H, VIRGL_RESOURCE_Y_0_TOP, "abandon");
    let (vbo, vbo_res, vbo_va) = mint_buffer_bo(w, ctx, "abandon vbo");
    rb_upload_triangles(w, ctx, vbo, vbo_va);
    let before = ctx_field(w, ctx, "fence-signaled");
    let mut st: Vec<u32> = Vec::new();
    subctx_preamble(&mut st);
    clear_stream(&mut st, res, 1, 0.0, 0.0, 1.0);
    draw_state_stream(&mut st, vbo_res, RB_W, RB_H);
    submit_stream(w, ctx, &st, "abandon prime");
    let _ = rb_wait_signaled(w, ctx, before, 1, libthyla_rs::time::Instant::now(), "abandon prime");
    let abandoned = |w: i64| {
        parse_field(&open_read_string(w, "ctl"), "abandoned")
            .unwrap_or_else(|| rb_incomplete("instrument:abandoned-count"))
    };
    // The slots a parked chain held must come back, not only its count.
    let f0 = fenced_free(w);
    if !write_ctl(w, "ctl", "warp-hold on") {
        rb_incomplete("instrument:abandon-hold");
    }
    let a0 = abandoned(w);
    let settle = rb_console_settle(tap);
    let r0 = stall_refusals();
    let (s0, _) = rb_queue_heavy(w, ctx, RB_ABANDON_SUBMITS, "abandon");
    let xfer = format!("transfer_from 0 0 0 0 {} {} 1 0 0 0", RB_W, RB_H);
    let (n, _) = write_behind_stall(w, &format!("ctx/{}/bo/{}/ctl", ctx, bo), xfer.as_bytes());
    if n != xfer.len() as i64 {
        rb_incomplete("instrument:abandon-transfer-refused");
    }
    let met = stall_refusals().wrapping_sub(r0);
    if !write_ctl(w, "ctl", "warp-abandon") {
        rb_incomplete("instrument:abandon");
    }
    let taken = abandoned(w).saturating_sub(a0);
    let t = libthyla_rs::time::Instant::now();
    let mut left = ctx_field(w, ctx, "fences-in-flight");
    while left != 0 && t.elapsed().as_millis() < 2000 {
        let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(50));
        left = ctx_field(w, ctx, "fences-in-flight");
    }
    let drained_ms = t.elapsed().as_millis() as u64;
    // Nothing may count while the context is poisoned: the abandoned chains are
    // not completions and the released readback never ran. A dense count past
    // s0 would answer a wait on the abandoned fence as if it had finished.
    let s1 = ctx_field(w, ctx, "fence-signaled");
    let poisoned = parse_field(&open_read_string(w, &format!("ctx/{}/ctl", ctx)), "poisoned").unwrap_or(99);
    let _ = write_ctl(w, "ctl", "warp-hold off");
    let mut healed = false;
    for _ in 0..400 {
        if parse_field(&open_read_string(w, &format!("ctx/{}/ctl", ctx)), "poisoned") == Some(0)
            && ctx_field(w, ctx, "fences-in-flight") == 0
            && fenced_free(w) >= f0
        {
            healed = true;
            break;
        }
        let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(50));
    }
    // Once the hold's replay vindicates the queue, the queue counts (exactly
    // three) and the released readback never does: its context's stream stays
    // over, so a later submit is refused rather than resumed over a readback
    // that never ran.
    let s2 = ctx_field(w, ctx, "fence-signaled");
    let f_end = fenced_free(w);
    let released = parse_field(&open_read_all(w, &format!("ctx/{}/ctl", ctx)), "released").unwrap_or(99);
    // Each of the latch's readers, by the answer only the latch gives. The
    // fence file answers EOF at once: nothing has read it, so a build without
    // the latch there answers the vindicated count instead, also at once. A
    // submit and `verify` are refused with E_IO, never a stall's EAGAIN (resent)
    // or an open refusal; without the latch both would be taken.
    let fence_path = format!("ctx/{}/fence", ctx);
    let fence_n = unsafe {
        let fd = t_open(w, fence_path.as_ptr(), fence_path.len(), T_OREAD);
        if fd < 0 {
            fd
        } else {
            let mut fb = [0u8; 64];
            let n = t_read(fd, fb.as_mut_ptr(), fb.len());
            t_close(fd);
            n
        }
    };
    let nop = [cmd0(VIRGL_CCMD_CLEAR, 0, 0).to_le_bytes()].concat();
    let submit_rc = write_rc(w, &format!("ctx/{}/submit", ctx), &nop);
    let verify_rc = write_rc(w, &format!("ctx/{}/ctl", ctx), b"verify");
    let refused_after = fence_n == 0 && submit_rc == Ok(-E_IO_RC) && verify_rc == Ok(-E_IO_RC);
    // Where a submit the seam took anyway went: the broker's own record of the
    // released chain parks and releases it (released +1), and without that
    // record too it runs and counts (fence-signaled +1). Read once the
    // context has nothing in flight.
    let t = libthyla_rs::time::Instant::now();
    while ctx_field(w, ctx, "fences-in-flight") != 0 && t.elapsed().as_millis() < 2000 {
        let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(50));
    }
    let s3 = ctx_field(w, ctx, "fence-signaled");
    let released3 = parse_field(&open_read_all(w, &format!("ctx/{}/ctl", ctx)), "released").unwrap_or(99);
    let _ = write_ctl(w, &format!("ctx/{}/ctl", ctx), "destroy");
    unsafe { t_close(w) };
    // Constructed only if the abandon took exactly the queue: a readback the
    // device had been handed would have been abandoned with it.
    if taken != RB_ABANDON_SUBMITS as u64 || poisoned != 1 || met != 0 {
        t_putstr(&format!(
            "warp-prove: C6-RB ABANDON attempt {} not constructed -- the abandon took {} chains (the queue is {}; one more means the readback was on the device, not parked), ctx poisoned {}, stall refusals on its sends {}; the console {}\n",
            k, taken, RB_ABANDON_SUBMITS, poisoned, met, settle.said()
        ));
        return None;
    }
    let due = RB_ABANDON_SUBMITS as u64;
    let ok = left == 0
        && healed
        && s1 == s0
        && s2 == s0 + due
        && released == 1
        && refused_after
        && s3 == s2
        && released3 == 1
        && f_end >= f0;
    t_putstr(&format!(
        "warp-prove: C6-RB ABANDON {} -- the readback parked behind {} abandoned chains; the ctx's fences in flight {} after {} ms (0 = the parked readback was released); fence-signaled +{} while poisoned (want +0), +{} after the vindication (want +{}, the queue alone); released {} (want 1); after the vindication the stream stays over: fence read {} (want 0, EOF), submit {:?}, verify {:?} (want Ok(-{}) each, E_IO); after them fence-signaled +{} released {} (after-submit S{}R{}; want S{}R1); fenced-free {} after, {} before{}\n",
        if ok { "PASS" } else { "FAIL" },
        RB_ABANDON_SUBMITS, left, drained_ms,
        s1.wrapping_sub(s0), s2.wrapping_sub(s0), due,
        released,
        fence_n, submit_rc, verify_rc, E_IO_RC,
        s3.wrapping_sub(s0), released3, s3.wrapping_sub(s0), released3, due,
        f_end, f0,
        if healed { "" } else { "; the lane did not heal after the hold came off" }
    ));
    Some(ok)
}

fn rb_incomplete(arm: &str) -> ! {
    t_putstr(&format!("warp-prove: C6-READBACK INCOMPLETE({})\n", arm));
    unsafe { t_exits(0) }
}

/// Mint a W x H B8G8R8A8 render-target BO with `flags` (Y_0_TOP makes it
/// NON-composable -- the readback arm's shape) and return (bo, res, va).
/// Mint a BO declaring `w`x`h` but backed by `size` bytes -- the shape the
/// follow-up round's F1 turned on. The create-time door has NO lower bound
/// (deliberately: Mesa's staging and MSAA paths declare one page for a real
/// texture), so this SUCCEEDS at create3d. The refusal that matters happens
/// later, at the READ gate in `gl_adoption`.
///
/// Deliberately does NOT open `map`: `b.dma_fd` is set SERVER-side inside
/// `wbo_create`, not by the client's map, so this BO still passes
/// `gl_adoption`'s `dma_fd >= 0` arm and the ONLY difference from the control
/// is `b.size`. If it refused on `dma_fd` instead, the leg would pass for the
/// wrong reason. (`gl_adoption` also pins `b.w == s.w && b.h == s.h`; if the
/// surface geometry ever diverges from RB_W/RB_H the CONTROL fails too and the
/// leg reports INSTRUMENT rather than a false PASS.)
fn mint_bo_wh_sized(root: i64, ctx: u32, w: u32, h: u32, flags: u32, size: u32, what: &str) -> u32 {
    let bo = match parse_u32_prefix(&open_read_string(root, &format!("ctx/{}/bo/new", ctx))) {
        Some(v) => v,
        None => fail(&format!("readback: bo/new for {}", what)),
    };
    let create = format!(
        "create3d {} {} {} {} {} 1 1 0 0 {} {}",
        PIPE_TEXTURE_2D, VIRGL_FORMAT_B8G8R8A8_UNORM, VIRGL_BIND_RENDER_TARGET, w, h, flags, size
    );
    if !write_ctl(root, &format!("ctx/{}/bo/{}/ctl", ctx, bo), &create) {
        fail(&format!("readback: create3d for {} (the door must ADMIT this)", what));
    }
    bo
}

fn mint_bo_wh(root: i64, ctx: u32, w: u32, h: u32, flags: u32, what: &str) -> (u32, u32, u64) {
    let bo = match parse_u32_prefix(&open_read_string(root, &format!("ctx/{}/bo/new", ctx))) {
        Some(v) => v,
        None => fail(&format!("readback: bo/new for {}", what)),
    };
    let create = format!(
        "create3d {} {} {} {} {} 1 1 0 0 {} {}",
        PIPE_TEXTURE_2D,
        VIRGL_FORMAT_B8G8R8A8_UNORM,
        VIRGL_BIND_RENDER_TARGET,
        w,
        h,
        flags,
        w * h * 4
    );
    if !write_ctl(root, &format!("ctx/{}/bo/{}/ctl", ctx, bo), &create) {
        fail(&format!("readback: create3d for {}", what));
    }
    let res = parse_field(&open_read_string(root, &format!("ctx/{}/bo/{}/info", ctx, bo)), "res")
        .unwrap_or_else(|| fail(&format!("readback: info `res` for {}", what))) as u32;
    let map_fd = unsafe {
        let p = format!("ctx/{}/bo/{}/map", ctx, bo);
        t_open(root, p.as_ptr(), p.len(), T_OREAD)
    };
    if map_fd < 0 {
        fail(&format!("readback: map open for {}", what));
    }
    let va = unsafe { t_weft_map(map_fd as u64, 0) };
    if va < 0 {
        fail(&format!("readback: weft_map for {}", what));
    }
    (bo, res, va as u64)
}

/// SET_FRAMEBUFFER_STATE(cbuf0 = `surf`) -- the surface object must exist.
fn fb_state_stream(st: &mut Vec<u32>, surf: u32) {
    st.push(cmd0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3));
    st.push(1); // nr_cbufs
    st.push(0); // zsurf
    st.push(surf);
}

/// CREATE_OBJECT(SHADER): the header + the TGSI text, encoded exactly as
/// Mesa's `virgl_encode_shader_state` does for a shader that fits one
/// packet (offset field = the whole text length incl. NUL, no CONT bit;
/// zero stream-out outputs; the text zero-padded to dwords).
fn shader_stream(st: &mut Vec<u32>, handle: u32, stage: u32, text: &str, num_tokens: u32) {
    let bytes = text.len() + 1; // incl. NUL
    let ndw = (bytes + 3) / 4;
    st.push(cmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SHADER, (5 + ndw) as u32));
    st.push(handle);
    st.push(stage);
    st.push(bytes as u32); // VIRGL_OBJ_SHADER_OFFSET_VAL(total len)
    st.push(num_tokens);
    st.push(0); // so num outputs
    let mut buf: Vec<u8> = text.as_bytes().to_vec();
    buf.push(0);
    while buf.len() % 4 != 0 {
        buf.push(0);
    }
    for c in buf.chunks(4) {
        st.push(u32::from_le_bytes([c[0], c[1], c[2], c[3]]));
    }
}

/// The draw state, once per ctx: shaders + vertex elements + blend / dsa /
/// rasterizer objects, bound; the vertex buffer (a full-screen triangle in
/// clip space, uploaded by the caller); the viewport. Layouts per
/// `virgl_protocol.h` and Mesa's `virgl_encode.c` (the tree at
/// ../mesa-thylacine), field for field.
fn draw_state_stream(st: &mut Vec<u32>, vbo_res: u32, w: u32, h: u32) {
    shader_stream(
        st,
        RB_H_VS,
        VIRGL_SHADER_VERTEX,
        "VERT\nDCL IN[0]\nDCL OUT[0], POSITION\n  0: MOV OUT[0], IN[0]\n  1: END\n",
        64,
    );
    shader_stream(
        st,
        RB_H_FS,
        VIRGL_SHADER_FRAGMENT,
        "FRAG\nDCL OUT[0], COLOR\nDCL CONST[0]\n  0: MOV OUT[0], CONST[0]\n  1: END\n",
        64,
    );
    // vertex elements: one vec4 float attribute from buffer 0
    st.push(cmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_VERTEX_ELEMENTS, 5));
    st.push(RB_H_VE);
    st.push(0); // src_offset
    st.push(0); // instance_divisor
    st.push(0); // vertex_buffer_index
    st.push(VIRGL_FORMAT_R32G32B32A32_FLOAT);
    // blend: rt0 colormask RGBA, nothing else
    st.push(cmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_BLEND, 8 + 3));
    st.push(RB_H_BLEND);
    st.push(0); // S0
    st.push(0); // S1
    st.push(0xf << 27); // S2 rt0: colormask 0xf
    for _ in 1..8 {
        st.push(0);
    }
    // dsa: everything off
    st.push(cmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_DSA, 5));
    st.push(RB_H_DSA);
    st.push(0); // S0
    st.push(0); // S1 stencil 0
    st.push(0); // S1 stencil 1
    st.push(0); // alpha ref
    // rasterizer: fill, no cull, half-pixel-center + bottom-edge-rule (GL)
    st.push(cmd0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_RASTERIZER, 9));
    st.push(RB_H_RS);
    st.push((1 << 1) | (1 << 29) | (1 << 30)); // S0: depth_clip, half_pixel_center, bottom_edge_rule
    st.push(1.0f32.to_bits()); // point size
    st.push(0); // sprite coord enable
    st.push(0); // S3
    st.push(1.0f32.to_bits()); // line width
    st.push(0); // offset units
    st.push(0); // offset scale
    st.push(0); // offset clamp
    // bind
    st.push(cmd0(VIRGL_CCMD_BIND_SHADER, 0, 2));
    st.push(RB_H_VS);
    st.push(VIRGL_SHADER_VERTEX);
    st.push(cmd0(VIRGL_CCMD_BIND_SHADER, 0, 2));
    st.push(RB_H_FS);
    st.push(VIRGL_SHADER_FRAGMENT);
    st.push(cmd0(VIRGL_CCMD_BIND_OBJECT, VIRGL_OBJECT_VERTEX_ELEMENTS, 1));
    st.push(RB_H_VE);
    st.push(cmd0(VIRGL_CCMD_BIND_OBJECT, VIRGL_OBJECT_BLEND, 1));
    st.push(RB_H_BLEND);
    st.push(cmd0(VIRGL_CCMD_BIND_OBJECT, VIRGL_OBJECT_DSA, 1));
    st.push(RB_H_DSA);
    st.push(cmd0(VIRGL_CCMD_BIND_OBJECT, VIRGL_OBJECT_RASTERIZER, 1));
    st.push(RB_H_RS);
    // vertex buffer 0: stride 16, offset 0, the raw device-global res id
    st.push(cmd0(VIRGL_CCMD_SET_VERTEX_BUFFERS, 0, 3));
    st.push(16);
    st.push(0);
    st.push(vbo_res);
    // viewport 0: scale (w/2, h/2, 0.5) translate (w/2, h/2, 0.5)
    st.push(cmd0(VIRGL_CCMD_SET_VIEWPORT_STATE, 0, 7));
    st.push(0); // start slot
    st.push((w as f32 / 2.0).to_bits());
    st.push((h as f32 / 2.0).to_bits());
    st.push(0.5f32.to_bits());
    st.push((w as f32 / 2.0).to_bits());
    st.push((h as f32 / 2.0).to_bits());
    st.push(0.5f32.to_bits());
}

/// One full-screen draw: the FS colour from an inline constant buffer
/// (SET_CONSTANT_BUFFER stage FS index 0, 4 floats), then DRAW_VBO of 3
/// vertices, TRIANGLES.
fn draw_stream(st: &mut Vec<u32>, r: f32, g: f32, b: f32) {
    draw_stream_instanced(st, r, g, b, 1);
}

fn draw_stream_instanced(st: &mut Vec<u32>, r: f32, g: f32, b: f32, instances: u32) {
    st.push(cmd0(VIRGL_CCMD_SET_CONSTANT_BUFFER, 0, 6));
    st.push(VIRGL_SHADER_FRAGMENT);
    st.push(0); // index
    st.push(r.to_bits());
    st.push(g.to_bits());
    st.push(b.to_bits());
    st.push(1.0f32.to_bits());
    st.push(cmd0(VIRGL_CCMD_DRAW_VBO, 0, 12));
    st.push(0); // start
    st.push((RB_TRIS * 3) as u32); // count
    st.push(MESA_PRIM_TRIANGLES);
    st.push(0); // indexed
    st.push(instances); // instance count
    st.push(0); // index bias
    st.push(0); // start instance
    st.push(0); // primitive restart
    st.push(0); // restart index
    st.push(0); // min index
    st.push(0xffff_ffff); // max index
    st.push(0); // count from so
}

/// The draw index encoded in the adopted BO's colour: draw `idx` of `total`
/// paints A (0, 1, (idx+1)/total) -- so the BLUE byte of any later readback
/// of A names the LAST draw that readback observed. `rb_index_of` decodes
/// it (B8G8R8A8: blue is the low byte).
fn rb_clear_color(idx: usize, total: usize) -> f32 {
    ((idx + 1) as f32) / (total as f32)
}
fn rb_index_of(px: u32, total: usize) -> i64 {
    let b = (px & 0xff) as f32 / 255.0;
    (b * total as f32 + 0.5) as i64 - 1
}

/// Queue `n` heavy submits on `ctx`: each RB_DRAWS_PER_SUBMIT full-screen
/// draws into the adopted BO (the framebuffer re-set at the head of each
/// submit), the colour index-encoded -- real rasterization on the CLIENT's
/// GL context that WRITES the adopted resource, ordered before any fence or
/// readback on that context, and nothing the driver can fold. Returns
/// (fence-signaled before the first submit, the Instant of the first).
fn rb_queue_heavy(
    root: i64,
    ctx: u32,
    n: usize,
    what: &str,
) -> (u64, libthyla_rs::time::Instant) {
    let before = ctx_field(root, ctx, "fence-signaled");
    let t0 = libthyla_rs::time::Instant::now();
    let total = n * RB_DRAWS_PER_SUBMIT;
    for i in 0..n {
        let mut st: Vec<u32> = Vec::new();
        fb_state_stream(&mut st, 1);
        for k in 0..RB_DRAWS_PER_SUBMIT {
            let idx = i * RB_DRAWS_PER_SUBMIT + k;
            draw_stream(&mut st, 0.0, 1.0, rb_clear_color(idx, total));
        }
        submit_stream(root, ctx, &st, &format!("{} heavy submit {}", what, i));
    }
    guard_not_poisoned(root, what);
    (before, t0)
}

/// Presents tapestryd has served, over every arm.
fn presents_served(tap: i64) -> u64 {
    let s = open_read_all(tap, "ctl");
    [
        "present-direct-gl",
        "present-direct-2d",
        "present-composed-bo",
        "present-composed-slot",
        "present-composed-cpu",
        "present-other",
        "present-poke-img",
    ]
    .iter()
    .map(|k| cost_line(&s, k).map_or(0, |(n, _, _)| n))
    .sum()
}

/// What a console settle saw, for an attempt's line.
struct Settle {
    quiet: bool,
    edge: bool,
    served: u64,
    waited_ms: u64,
}

impl Settle {
    fn said(&self) -> String {
        match (self.quiet, self.edge) {
            (true, true) => format!("settled, the window opening on a present {} ms in", self.waited_ms),
            (true, false) => format!("was still (no present in {} ms)", RB_SETTLE_EDGE_MS),
            (false, _) => format!("never went quiet in {} ms", RB_SETTLE_BOUND_MS),
        }
    }
}

/// A window whose sends must meet no engine stall opens just after a console
/// present: every present is a sync step, and one that waits behind the deep
/// queue past its deadline stalls the engine and drains the queue under the
/// sends. Aurora paints every say (this process's before each window, and the
/// console's own about what that painting did) and flips its cursor on its own
/// frames, so the console first goes quiet and the window then opens on the
/// next present, a flip: the one after it is a cursor period away, longer
/// than the sends. Nothing is said in here.
fn rb_console_settle(tap: i64) -> Settle {
    let t0 = libthyla_rs::time::Instant::now();
    let mut served = presents_served(tap);
    let mut since = libthyla_rs::time::Instant::now();
    let mut quiet = false;
    while (t0.elapsed().as_millis() as u64) < RB_SETTLE_BOUND_MS {
        let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(5));
        let n = presents_served(tap);
        if n != served {
            served = n;
            since = libthyla_rs::time::Instant::now();
        } else if since.elapsed().as_millis() as u64 >= RB_SETTLE_QUIET_MS {
            quiet = true;
            break;
        }
    }
    let te = libthyla_rs::time::Instant::now();
    let mut edge = false;
    while (te.elapsed().as_millis() as u64) < RB_SETTLE_EDGE_MS {
        let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(2));
        let n = presents_served(tap);
        if n != served {
            served = n;
            edge = true;
            break;
        }
    }
    Settle { quiet, edge, served, waited_ms: t0.elapsed().as_millis() as u64 }
}

/// Wait until this ctx's fence-signaled has advanced by `n` (bounded);
/// returns the wall since `t0` when it did -- the queue's own retire time,
/// the diagnostic that says whether a queue existed at all.
fn rb_wait_signaled(
    root: i64,
    ctx: u32,
    before: u64,
    n: u64,
    t0: libthyla_rs::time::Instant,
    what: &str,
) -> u64 {
    let tw = libthyla_rs::time::Instant::now();
    loop {
        if ctx_field(root, ctx, "fence-signaled") >= before + n {
            return t0.elapsed().as_millis() as u64;
        }
        if ctx_field(root, ctx, "poisoned") != 0 {
            rb_incomplete(&format!("poisoned:{}", what));
        }
        if tw.elapsed().as_millis() as u64 > RB_LAND_BOUND_MS {
            rb_incomplete(&format!("fences-never-landed:{}", what));
        }
        let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(5));
    }
}

/// RB_TRIS full-screen triangles per draw (the same clip-space triangle RB_TRIS
/// times): RB_TRIS x the fill per DRAW_VBO at the same stream size, so the queue
/// is deep enough that the stall remaining when the readback is issued -- after
/// the ~130-290 ms the eight 24 KiB Twrites themselves take -- clears the DEEP
/// floor with margin (run 5: a 415 ms queue left 88 ms once).
fn rb_upload_triangles(warp: i64, ctx: u32, vbo: u32, vbo_va: u64) {
    let tri: [f32; 12] = [-1.0, -1.0, 0.0, 1.0, 3.0, -1.0, 0.0, 1.0, -1.0, 3.0, 0.0, 1.0];
    for t in 0..RB_TRIS {
        for (i, v) in tri.iter().enumerate() {
            unsafe {
                core::ptr::write_volatile((vbo_va + ((t * 12 + i) as u64) * 4) as *mut u32, v.to_bits())
            };
        }
    }
    let before = ctx_field(warp, ctx, "fence-signaled");
    let up = format!("transfer_to 0 0 0 0 {} 1 1 0 0 0", RB_TRIS * 48);
    let (n, refused) = write_behind_stall(warp, &format!("ctx/{}/bo/{}/ctl", ctx, vbo), up.as_bytes());
    if n != up.len() as i64 {
        rb_incomplete("instrument:vbo-upload");
    }
    note_stall_refusals("vbo upload", refused);
    let _ = rb_wait_signaled(warp, ctx, before, 1, libthyla_rs::time::Instant::now(), "vbo-upload");
}

/// Write one whole command to a warp file, and say whether the broker took it. EAGAIN
/// is the broker's stall (GPU-DESIGN 4.5.15), which ends by itself, and a client
/// keeping a queue full causes it; anything else fails this program.
fn write_unless_behind(root: i64, path: &str, bytes: &[u8]) -> bool {
    let fd = unsafe { t_open(root, path.as_ptr(), path.len(), T_OWRITE) };
    if fd < 0 {
        fail(&format!("chain-hold: open {}", path));
    }
    let n = unsafe { t_write(fd, bytes.as_ptr(), bytes.len()) };
    unsafe { t_close(fd) };
    if n == -11 {
        return false;
    }
    if n != bytes.len() as i64 {
        fail(&format!("chain-hold: {} refused ({})", path, n));
    }
    true
}

/// Keep `CHAIN_HOLD_DEPTH` heavy submits on the device for `argv[2]` seconds, and
/// say when the first full queue has turned over and refilled, read from the
/// broker's own fence count. With `readback` as `argv[3]`, one transfer of the busy
/// target also stays in flight at the tail.
fn chain_hold() -> i64 {
    let secs: u64 = libthyla_rs::env::args()
        .get_str(2)
        .and_then(|s| s.parse().ok())
        .unwrap_or_else(|| fail("chain-hold <seconds> [readback|deep]"));
    let mode = libthyla_rs::env::args().get_str(3);
    let readback = mode == Some("readback");
    let (depth, instances) = if mode == Some("deep") {
        (CHAIN_HOLD_DEEP_DEPTH, CHAIN_HOLD_DEEP_INSTANCES)
    } else {
        (CHAIN_HOLD_DEPTH, 1)
    };
    let warp = warp_connect("chain-hold");
    let ctx = mint_ctx(warp, "chain-hold");
    let (bo, res, _) = mint_bo_wh(warp, ctx, RB_W, RB_H, VIRGL_RESOURCE_Y_0_TOP, "chain-hold target");
    let (vbo, vbo_res, vbo_va) = mint_buffer_bo(warp, ctx, "chain-hold vbo");
    rb_upload_triangles(warp, ctx, vbo, vbo_va);
    let before = ctx_field(warp, ctx, "fence-signaled");
    let mut prime: Vec<u32> = Vec::new();
    subctx_preamble(&mut prime);
    clear_stream(&mut prime, res, 1, 1.0, 0.0, 0.0);
    draw_state_stream(&mut prime, vbo_res, RB_W, RB_H);
    submit_stream(warp, ctx, &prime, "chain-hold prime");
    guard_not_poisoned(warp, "chain-hold prime");
    let _ = rb_wait_signaled(warp, ctx, before, 1, libthyla_rs::time::Instant::now(), "chain-hold prime");
    let base = ctx_field(warp, ctx, "fence-signaled");
    let submit = format!("ctx/{}/submit", ctx);
    let bo_ctl = format!("ctx/{}/bo/{}/ctl", ctx, bo);
    let xfer = format!("transfer_from 0 0 0 0 {} {} 1 0 0 0", RB_W, RB_H);
    let mut sent = 0u64;
    let mut refused = 0u64;
    let mut read_at: Option<u64> = None;
    let mut said = false;
    // Why a run never reports DEEP: readbacks accepted / refused, draws refused, the
    // deepest queue seen behind an accepted readback, and per readback (first 8) the
    // fences retired when it was issued and how long it stood.
    let mut rb_ok = 0u64;
    let mut rb_refused = 0u64;
    let mut out_rb_max = 0u64;
    let mut rb_retired: Vec<u64> = Vec::new();
    let mut rb_stood: Vec<u64> = Vec::new();
    let mut rb_t: Option<libthyla_rs::time::Instant> = None;
    let t0 = libthyla_rs::time::Instant::now();
    while t0.elapsed().as_secs() < secs {
        let retired = ctx_field(warp, ctx, "fence-signaled") - base;
        if read_at.is_some_and(|at| retired > at) {
            read_at = None;
            if let Some(t) = rb_t.take() {
                if rb_stood.len() < 8 {
                    rb_stood.push(t.elapsed().as_millis() as u64);
                }
            }
        }
        if read_at.is_some() && sent - retired > out_rb_max {
            out_rb_max = sent - retired;
        }
        if sent - retired < depth {
            let mut st: Vec<u32> = Vec::new();
            fb_state_stream(&mut st, 1);
            for k in 0..RB_DRAWS_PER_SUBMIT {
                draw_stream_instanced(&mut st, 0.0, 1.0, rb_clear_color(k, RB_DRAWS_PER_SUBMIT), instances);
            }
            let bytes: Vec<u8> = st.iter().flat_map(|d| d.to_le_bytes()).collect();
            if write_unless_behind(warp, &submit, &bytes) {
                sent += 1;
            } else {
                refused += 1;
                let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(5));
            }
        } else if readback && read_at.is_none() {
            if write_unless_behind(warp, &bo_ctl, xfer.as_bytes()) {
                read_at = Some(sent);
                sent += 1;
                rb_ok += 1;
                rb_t = Some(libthyla_rs::time::Instant::now());
                if rb_retired.len() < 8 {
                    rb_retired.push(retired);
                }
            } else {
                refused += 1;
                rb_refused += 1;
                let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(5));
            }
        } else {
            if !said && retired >= depth {
                said = true;
                t_putstr(&format!(
                    "warp-prove: CHAIN-HOLD DEEP ctx {} sent {} retired {} refused {} after {} ms\n",
                    ctx,
                    sent,
                    retired,
                    refused,
                    t0.elapsed().as_millis()
                ));
            }
            let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(5));
        }
    }
    if readback {
        t_putstr(&format!(
            "warp-prove: CHAIN-HOLD TRACE ctx {} deep {} readbacks {} refused {} draws-refused {} deepest-behind-readback {} retired-at-readback {:?} readback-stood-ms {:?}\n",
            ctx,
            said as u32,
            rb_ok,
            rb_refused,
            refused - rb_refused,
            out_rb_max,
            rb_retired,
            rb_stood
        ));
    }
    t_putstr(&format!(
        "warp-prove: CHAIN-HOLD END ctx {} sent {} refused {} after {} ms\n",
        ctx,
        sent,
        refused,
        t0.elapsed().as_millis()
    ));
    0
}

fn observe_readback() {
    use tapestry::Surface;
    t_putstr("warp-prove: C6-READBACK starting (the compositor readback arm under a deep queue)\n");

    // The two console-side surfaces. A hosts the GL adoption (the readback
    // arm's target); B is the plain 2D bystander whose presents queue behind
    // the device. Both are ordinary tapestry clients of this process (one
    // conn each -- libtapestry opens per surface).
    let mut a = match Surface::open(RB_W, RB_H) {
        Ok(s) => s,
        Err(_) => rb_incomplete("instrument:surface-a"),
    };
    let mut b = match Surface::open(256, 256) {
        Ok(s) => s,
        Err(_) => rb_incomplete("instrument:surface-b"),
    };
    for px in b.pixels().iter_mut() {
        *px = 0xFF20_4060;
    }
    if b.present(None).is_err() {
        rb_incomplete("instrument:present-b");
    }

    // The warp side: one ctx, the adopted BO (Y_0_TOP: NOT composable, so
    // the blit arm cannot take it -- `composable` in tapestryd) and a
    // same-shape scratch partner for the heavy blits.
    let warp = warp_connect("readback");
    let ctx = mint_ctx(warp, "readback");
    let (bo, res, va) = mint_bo_wh(warp, ctx, RB_W, RB_H, VIRGL_RESOURCE_Y_0_TOP, "adopted");
    // The vertex buffer: one clip-space full-screen triangle, uploaded
    // through the fenced transfer verb (the C0-F1 leg's buffer shape).
    let (vbo, vbo_res, vbo_va) = mint_buffer_bo(warp, ctx, "readback vbo");
    rb_upload_triangles(warp, ctx, vbo, vbo_va);
    let tap = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv/tapestry".as_ptr(), 13, T_OREAD) };
    if tap < 0 {
        rb_incomplete("instrument:tapestry-ctl");
    }
    if rb_census(warp).is_none() {
        // A pre-C-6 tapestryd carries no `rb-*` census: ABSENT, not 0.
        rb_incomplete("instrument:comp-rb");
    }
    if cost_line(&open_read_all(tap, "ctl"), "readback-wait").is_none() {
        rb_incomplete("instrument:readback-wait");
    }

    // The mutual adoption: the surface names the ctx, the ctx consents
    // naming the surface incarnation + BO back.
    if a.surface_ctl(&format!("glsrc {}", ctx)).is_err() {
        rb_incomplete("instrument:glsrc");
    }
    if !write_ctl(warp, &format!("ctx/{}/ctl", ctx), &format!("present-to {} {}", a.id, bo)) {
        rb_incomplete("instrument:present-to");
    }

    // FOLLOW-UP ROUND F1's owed regression, at the gate that actually carries
    // it. The C-6b close guarded the read-overrun at the create-time DOOR and
    // that brace was removed -- it refused legitimate Mesa staging/MSAA
    // resources, which declare one page for a real texture. The bound lives at
    // the READ gate (`gl_adoption`: `b.size >= b.w * b.h * 4`), so that is
    // where the test has to look.
    //
    // Asserting the door would prove nothing now, and asserting "it did not
    // crash" proves nothing ever. The observable is `rb-issued`: an adoption
    // the read gate refuses issues NO compositor readback. The CONTROL is the
    // correctly-backed BO one variable away -- without it this leg passes just
    // as well against a compositor that never issues a readback at all, and
    // "the undersized one was refused" would be true for the wrong reason.
    let guard_pass = {
        let short_bo = mint_bo_wh_sized(
            warp, ctx, RB_W, RB_H, VIRGL_RESOURCE_Y_0_TOP, 4096, "undersized",
        );
        let i0 = match rb_census(warp) { Some(c) => c.issued, None => rb_incomplete("instrument:guard-census") };
        if !write_ctl(warp, &format!("ctx/{}/ctl", ctx), &format!("present-to {} {}", a.id, short_bo)) {
            rb_incomplete("instrument:guard-present-to");
        }
        if a.present(None).is_err() {
            rb_incomplete("instrument:guard-present");
        }
        let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(200));
        let i1 = match rb_census(warp) { Some(c) => c.issued, None => rb_incomplete("instrument:guard-census") };

        // Restore the correctly-backed adoption and prove the SAME sequence
        // does move it -- so the negative above is about the SIZE and not
        // about presents being inert here.
        if !write_ctl(warp, &format!("ctx/{}/ctl", ctx), &format!("present-to {} {}", a.id, bo)) {
            rb_incomplete("instrument:guard-restore");
        }
        if a.present(None).is_err() {
            rb_incomplete("instrument:guard-present2");
        }
        let mut i2 = i1;
        for _ in 0..40 {
            let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(50));
            i2 = match rb_census(warp) { Some(c) => c.issued, None => rb_incomplete("instrument:guard-census") };
            if i2 > i1 { break; }
        }
        if i1 != i0 {
            t_putstr(&format!(
                "warp-prove: C6-RB GUARD FAIL -- the compositor issued a readback ({}->{}) of a \
                 BO declaring {}x{} backed by 4096 bytes; the read gate did not refuse it and \
                 the compose would read {} bytes out of one page (round F1)\n",
                i0, i1, RB_W, RB_H, RB_W * RB_H * 4
            ));
            false
        } else if i2 <= i1 {
            t_putstr(
                "warp-prove: C6-RB GUARD INSTRUMENT -- the CONTROL never issued either; presents \
                 are not reaching the readback arm at all, so the refusal above means nothing\n",
            );
            false
        } else {
            // QUIESCE before returning. The control above leaves a readback IN
            // FLIGHT, and the ARM leg that runs next asserts `landed >
            // c0.landed` after its own present -- which OUR readback landing
            // would satisfy. A leg that leaves work in flight makes the next
            // leg pass for the wrong reason; drain it here rather than hope
            // the ordering holds.
            for _ in 0..40 {
                let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(50));
                // `rb-slot` IS the in-flight signal (0 free / 1 busy / 2
                // poisoned) -- a terminal-count comparison would need
                // `rb-dropped`, which this census struct does not carry.
                match rb_census(warp) {
                    Some(c) if c.slot == 0 => break,
                    Some(_) => {}
                    None => rb_incomplete("instrument:guard-census"),
                }
            }
            t_putstr(&format!(
                "warp-prove: C6-RB GUARD PASS -- an undersized adoption issued NO readback \
                 (rb-issued {} unchanged) while the correctly-backed twin issued one ({}->{}) \
                 through the SAME present: the read gate discriminates on SIZE\n",
                i0, i1, i2
            ));
            true
        }
    };
    if !guard_pass {
        rb_incomplete("guard");
    }

    // Prime the stream (sub-ctx + a surface over the BO + one clear + the
    // whole draw state), so the BO holds a frame and vrend's context state
    // exists before the heavy draws. Then ask the #240 detector: a stream
    // vrend REJECTS is accepted, its fences retire, and nothing runs -- so
    // an illegal prime would read as a light queue three arms later
    // instead of naming itself here.
    {
        let before = ctx_field(warp, ctx, "fence-signaled");
        let mut st: Vec<u32> = Vec::new();
        subctx_preamble(&mut st);
        clear_stream(&mut st, res, 1, 1.0, 0.0, 0.0);
        draw_state_stream(&mut st, vbo_res, RB_W, RB_H);
        submit_stream(warp, ctx, &st, "readback prime");
        guard_not_poisoned(warp, "readback prime");
        let _ = rb_wait_signaled(warp, ctx, before, 1, libthyla_rs::time::Instant::now(), "prime");
        if !write_ctl(warp, &format!("ctx/{}/ctl", ctx), "verify") {
            rb_incomplete("instrument:verify-refused");
        }
        let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(50));
        let c = open_read_string(warp, &format!("ctx/{}/ctl", ctx));
        let rejected = parse_field(&c, "stream-rejected").unwrap_or(99);
        let ok = parse_field(&c, "verify-ok").unwrap_or(0);
        if rejected != 0 || ok == 0 {
            t_putstr(&format!(
                "warp-prove: C6-RB PRIME -- stream-rejected {} verify-ok {}: the draw-state stream was refused by vrend (illegal encoding), nothing downstream can run\n",
                rejected, ok
            ));
            rb_incomplete("prime-rejected");
        }
    }
    // Let the compositor settle: B's present may have issued a deferred
    // health read (HEALTH_PERIOD ticks later); a sync step of the console's
    // own during LIVE would inherit the stall and be read as the console
    // waiting.
    let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(2000));

    let mut verdict_ok = true;
    let mut first_bad: Option<&'static str> = None;
    let mut record = |ok: bool, arm: &'static str| {
        if !ok {
            verdict_ok = false;
            if first_bad.is_none() {
                first_bad = Some(arm);
            }
        }
    };

    // ---- ARM: a present of the adoption on an IDLE queue issues + lands.
    let c0 = rb_census(warp).unwrap();
    let (g0, _cpu0) = composed_census(&open_read_all(tap, "ctl")).unwrap_or((0, 0));
    if a.present(None).is_err() {
        rb_incomplete("instrument:present-a");
    }
    let t0 = libthyla_rs::time::Instant::now();
    let mut arm_ok = false;
    let mut composable = false;
    while t0.elapsed().as_millis() < 5000 {
        let c = rb_census(warp).unwrap();
        if c.landed > c0.landed {
            arm_ok = true;
            break;
        }
        let (g, _) = composed_census(&open_read_all(tap, "ctl")).unwrap_or((0, 0));
        if g > g0 && c.issued == c0.issued {
            composable = true;
            break;
        }
        let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(5));
    }
    if composable {
        t_putstr("warp-prove: C6-RB ARM FAIL -- the adopted BO was GPU-composed (blit arm), not read back\n");
        rb_incomplete("bo-composable");
    }
    let c1 = rb_census(warp).unwrap();
    t_putstr(&format!(
        "warp-prove: C6-RB ARM {} -- idle-queue present: comp-rb issued {}->{} landed {}->{}\n",
        if arm_ok { "PASS" } else { "FAIL" },
        c0.issued, c1.issued, c0.landed, c1.landed
    ));
    if !arm_ok {
        rb_incomplete("arm-never-landed");
    }
    record(true, "arm");

    // ---- LIVE + DEEP, RB_LIVE_ROUNDS times: a deep queue, then the present
    // that ISSUES the compositor readback -- timed -- then the flight
    // watched until the readback lands. LIVE is the issuing present's own
    // latency: under the pre-C-6 arm it IS the wait (the whole queue);
    // under C-6 it publishes a fenced command and returns. The presents and
    // ctl reads DURING the flight are reported, not judged: any other
    // client's sync step in that window (the console renderer's ~2 Hz
    // cursor blink is one) inherits the device stall on QEMU/virgl and the
    // single-threaded loop waits there for everyone -- consequence 2 of
    // GPU-DESIGN 4.5.13, F2b's territory, and run 4 of this leg measured
    // exactly that (a 140 ms second present inside a 168 ms flight).
    let mut deep_ok = true;
    let mut live_ok = true;
    let mut flight_present_max = 0u64;
    let mut flight_ctl_max = 0u64;
    let mut rounds_done = 0usize;
    let mut unconstructed = 0usize;
    let total = RB_LIVE_SUBMITS * RB_DRAWS_PER_SUBMIT;
    for attempt in 0..RB_LIVE_ATTEMPTS {
        if rounds_done >= RB_LIVE_ROUNDS {
            break;
        }
        let round = attempt;
        // Sentinel the pixel the compositor readback will overwrite: after
        // it lands, the BLUE byte names the last draw the readback observed.
        unsafe { core::ptr::write_volatile(va as *mut u32, SENTINEL) };
        let _ = rb_console_settle(tap);
        let tap0 = open_read_all(tap, "ctl");
        let (rw_n0, rw_sum0, _) = cost_line(&tap0, "readback-wait").unwrap();
        let others0 = others_census(&tap0);
        let (live_before, live_t0) = rb_queue_heavy(warp, ctx, RB_LIVE_SUBMITS, "live");
        let cl = rb_census(warp).unwrap();
        // How much of the round's queue had retired when the readback was
        // issued, read off the ctx's own fence count just before the present.
        let retired_at_issue = ctx_field(warp, ctx, "fence-signaled").saturating_sub(live_before);
        // The issue time is stamped BEFORE the present: the issue is the
        // dispatch's first act, and a present that WAITS for the readback
        // (the pre-C-6 arm; sabotage S1) must read as a slow issuing
        // present, not as a readback issued late into a drained queue.
        let issued_at_ms = live_t0.elapsed().as_millis() as u64;
        let tp = libthyla_rs::time::Instant::now();
        if a.present(None).is_err() {
            rb_incomplete("instrument:present-a-live");
        }
        let first_ms = tp.elapsed().as_millis() as u64;
        let mut max_present_ms = 0u64;
        let mut max_ctl_ms = 0u64;
        let mut presents = 0u64;
        let mut slot_poisoned = false;
        let tl = libthyla_rs::time::Instant::now();
        let mut landed_seen = false;
        let mut abandoned = false;
        let mut fence_ms: [u64; RB_LIVE_SUBMITS] = [0; RB_LIVE_SUBMITS];
        let mut fences_seen = 0usize;
        let mut landed_at_ms = 0u64;
        while (tl.elapsed().as_millis() as u64) < RB_LAND_BOUND_MS {
            let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(5));
            let t = libthyla_rs::time::Instant::now();
            if a.present(None).is_err() {
                rb_incomplete("instrument:present-a-loop");
            }
            let ms = t.elapsed().as_millis() as u64;
            presents += 1;
            if ms > max_present_ms {
                max_present_ms = ms;
            }
            let t = libthyla_rs::time::Instant::now();
            let c = rb_census(warp).unwrap();
            let sig = ctx_field(warp, ctx, "fence-signaled");
            let ms = t.elapsed().as_millis() as u64;
            if ms > max_ctl_ms {
                max_ctl_ms = ms;
            }
            let now_ms = live_t0.elapsed().as_millis() as u64;
            while fences_seen < RB_LIVE_SUBMITS && sig >= live_before + fences_seen as u64 + 1 {
                fence_ms[fences_seen] = now_ms;
                fences_seen += 1;
            }
            if c.slot == 2 {
                slot_poisoned = true;
            }
            if c.abandoned > cl.abandoned {
                abandoned = true;
                break;
            }
            if c.landed > cl.landed {
                landed_seen = true;
                landed_at_ms = now_ms;
                break;
            }
        }
        let observed_px = unsafe { core::ptr::read_volatile(va as *const u32) };
        let observed_idx = if observed_px == SENTINEL { -2 } else { rb_index_of(observed_px, total) };
        // The queue must be fully retired before the next round (and before
        // any verdict that reads readback-wait, which is written at retire).
        let queue_ms = rb_wait_signaled(warp, ctx, live_before, RB_LIVE_SUBMITS as u64, live_t0, "live");
        let tap1 = open_read_all(tap, "ctl");
        let (rw_n1, rw_sum1, _) = cost_line(&tap1, "readback-wait").unwrap();
        let round_wait_ms = rw_sum1.saturating_sub(rw_sum0) / 1000;
        let others1 = others_census(&tap1);
        let wctl = open_read_all(warp, "ctl");
        let comp_parked = parse_field(&wctl, "rb-last-parked-ms").unwrap_or(u64::MAX);
        let comp_exec = parse_field(&wctl, "rb-last-exec-ms").unwrap_or(u64::MAX);
        if abandoned {
            t_putstr("warp-prove: C6-RB LIVE FAIL -- the compositor readback was ABANDONED (30 s)\n");
            rb_incomplete("abandoned");
        }
        if !landed_seen {
            t_putstr("warp-prove: C6-RB LIVE FAIL -- the compositor readback never landed inside the bound\n");
            rb_incomplete("live-never-landed");
        }
        // CONSTRUCTED: the readback was issued behind the WHOLE round's queue,
        // by the fence timeline -- none of its submits had retired. The sends
        // can be blocked for most of the queue by another client's present
        // landing behind it (its egl-headless flush is a screen readback
        // queued behind the client's draws on V3D's one FIFO -- run 6 measured
        // 478 / 794 / 1062 ms to send eight Twrites). The old check compared
        // the issue time with the time the LAST retire was observed, and on
        // the Pi it passed a readback issued after three of four submits had
        // retired: the holds delayed the sends, then the observation. A
        // readback issued into a draining queue tests nothing and is retried,
        // never judged.
        let constructed = retired_at_issue == 0;
        if !constructed {
            unconstructed += 1;
            t_putstr(&format!(
                "warp-prove: C6-RB ROUND {} UNCONSTRUCTED -- {} of the {} submits had retired when the readback was issued {} ms into a {} ms queue (sends blocked behind another client's present); retrying\n",
                round, retired_at_issue, RB_LIVE_SUBMITS, issued_at_ms, queue_ms
            ));
            continue;
        }
        rounds_done += 1;
        // ROUND F8 [P3]: `round_wait_ms` was a SUM over `rw_n1 - rw_n0`
        // retires asserted against a per-readback threshold -- a figure no
        // single readback earned could satisfy it.
        //
        // The first cut of this fix required EXACTLY ONE retire per round and
        // the gate went RED on a healthy build: measured here, every round
        // retires TWO (`comp-rb landed 1->7` across three rounds). The flight
        // loop's later presents each request a readback, and the pump issues
        // the next the moment the first lands, so a second lands inside the
        // same window. Requiring one was a claim about the mechanism's
        // scheduling, not about the property under test -- and it was wrong.
        //
        // The MEAN is the honest statistic: a mean at or above the threshold
        // implies at least one readback reached it, whatever the count, and
        // it correctly rejects the case the sum admitted (one long readback
        // plus one instant one averages below). The pixel witness stays --
        // it is what proves the wait was on the queue's LAST draw rather than
        // merely long.
        let round_n = rw_n1.saturating_sub(rw_n0);
        let round_mean_ms = if round_n > 0 { round_wait_ms / round_n } else { 0 };
        let round_deep = round_n > 0 && round_mean_ms >= RB_DEEP_MS && observed_idx >= total as i64 - 2;
        let round_live = first_ms < RB_LIVE_MS && !slot_poisoned;
        deep_ok &= round_deep;
        live_ok &= round_live;
        if max_present_ms > flight_present_max {
            flight_present_max = max_present_ms;
        }
        if max_ctl_ms > flight_ctl_max {
            flight_ctl_max = max_ctl_ms;
        }
        t_putstr(&format!(
            "warp-prove: C6-RB ROUND {} -- issuing present {} ms; readback issued at {} landed at {} (ms since the first heavy submit), observed draw {} of {} (pixel {:#010x}); readback-wait +{} ms over {} retires (mean {} ms); the {} heavy submits retired in {} ms, fences at {:?}; the last compositor readback parked {} ms and ran {} ms; during the flight: {} presents max {} ms, ctl reads max {} ms; other console work this round: slot-presents +{} cpu-presents +{} bo-presents +{} xfers +{} pushes +{} flushes +{} health +{}; deep {} live {}\n",
            round, first_ms, issued_at_ms, landed_at_ms, observed_idx, total, observed_px, round_wait_ms,
            round_n, round_mean_ms,
            RB_LIVE_SUBMITS, queue_ms, fence_ms, comp_parked, comp_exec, presents, max_present_ms, max_ctl_ms,
            others1[0] - others0[0], others1[1] - others0[1], others1[2] - others0[2],
            others1[3] - others0[3], others1[4] - others0[4], others1[5] - others0[5], others1[6] - others0[6],
            round_deep as u32, round_live as u32
        ));
    }
    let (_, _, rw_max_all) = cost_line(&open_read_all(tap, "ctl"), "readback-wait").unwrap();
    let c2 = rb_census(warp).unwrap();
    if rounds_done < RB_LIVE_ROUNDS {
        t_putstr(&format!(
            "warp-prove: C6-RB DEEP INSTRUMENT -- only {} of {} rounds could be constructed in {} attempts ({} unconstructed: the sends kept landing behind another client's present)\n",
            rounds_done, RB_LIVE_ROUNDS, RB_LIVE_ATTEMPTS, unconstructed
        ));
        rb_incomplete("deep-unconstructed");
    }
    t_putstr(&format!(
        "warp-prove: C6-RB DEEP {} -- {} constructed rounds ({} unconstructed retried): the round's MEAN readback wait was >= {} ms -- so at least one readback in each round waited that long for its queue (readback-wait max {} ms) and observed the queue's LAST draw: the device paid the queue\n",
        if deep_ok { "PASS" } else { "FAIL" },
        rounds_done, unconstructed, RB_DEEP_MS, rw_max_all / 1000
    ));
    if !deep_ok {
        // Diagnose the light queue: did vrend latch the stream off (#240 --
        // submits accepted, fences retiring, nothing running), or did the
        // work simply cost nothing? The detector answers the first.
        let _ = write_ctl(warp, &format!("ctx/{}/ctl", ctx), "verify");
        let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(50));
        let c = open_read_string(warp, &format!("ctx/{}/ctl", ctx));
        t_putstr(&format!(
            "warp-prove: C6-RB DEEP diagnosis -- stream-rejected {} verify-seq {} verify-ok {} (a rejected stream retires its fences without running)\n",
            parse_field(&c, "stream-rejected").unwrap_or(99),
            parse_field(&c, "verify-seq").unwrap_or(99),
            parse_field(&c, "verify-ok").unwrap_or(99)
        ));
    }
    t_putstr(&format!(
        "warp-prove: C6-RB LIVE {} -- {} constructed rounds: the present that ISSUES the compositor readback of a deep queue returned inside {} ms every time (the pre-C-6 arm returns after the whole wait); comp-rb coalesced {}->{} landed {}->{}\n",
        if live_ok { "PASS" } else { "FAIL" },
        rounds_done, RB_LIVE_MS, c1.coalesced, c2.coalesced, c1.landed, c2.landed
    ));
    t_putstr(&format!(
        "warp-prove: C6-RB FLIGHT REPORT -- while a compositor readback of a deep queue was in flight: the adopting surface's later presents max {} ms, warp ctl reads max {} ms (data: another client's sync step in that window inherits the device stall on this host and the loop waits there -- 4.5.13 consequence 2)\n",
        flight_present_max, flight_ctl_max
    ));
    record(deep_ok, "deep");
    record(live_ok, "live");

    // ---- DEADLINE + F2B (operator vote 81): the CLIENT's own readback of its
    // busy BO, one later submit in a colour no queue index uses, then B's
    // presents. Lictor must park the readback behind the queue and the later
    // submit behind the readback; B's sync steps wait on the queue through
    // their own fences, so one stalls and is refused, and the engine revives.
    // The backing is re-sentinelled first and no compositor readback may land
    // in it meanwhile, so the pixel ORDER reads is the client readback's: LIVE
    // left the queue's last-draw colour there.
    // A stall met by the queue's or the readback's sends (another client's sync
    // step waiting behind the queue) lets the queue retire before the readback
    // is taken, so it publishes unparked: that attempt is unconstructed, and the
    // leg runs again rather than judging it.
    let xfer = format!("transfer_from 0 0 0 0 {} {} 1 0 0 0", RB_W, RB_H);
    let mut dl_try = 0u32;
    let (dl_before, dl_t0, dl_c0) = loop {
        dl_try += 1;
        let mut dl_c0 = rb_census(warp).unwrap_or_else(|| rb_incomplete("instrument:deadline-census"));
        for _ in 0..40 {
            if dl_c0.slot == 0 {
                break;
            }
            let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(50));
            dl_c0 = rb_census(warp).unwrap_or_else(|| rb_incomplete("instrument:deadline-census"));
        }
        if dl_c0.slot != 0 {
            rb_incomplete("instrument:deadline-comp-rb-in-flight");
        }
        unsafe { core::ptr::write_volatile(va as *mut u32, SENTINEL) };
        let settle = rb_console_settle(tap);
        let r0 = stall_refusals();
        let (dl_before, dl_t0) = rb_queue_heavy(warp, ctx, RB_DEADLINE_SUBMITS, "deadline");
        let (n, refused) = write_behind_stall(warp, &format!("ctx/{}/bo/{}/ctl", ctx, bo), xfer.as_bytes());
        if n != xfer.len() as i64 {
            rb_incomplete("instrument:f2b-transfer-refused");
        }
        note_stall_refusals("deadline readback", refused);
        let met = stall_refusals().wrapping_sub(r0);
        // DEEP's rule, by the fence timeline: none of the queue may have retired
        // when the readback was taken. A hold on the sends without a refusal (a
        // present's flush queued behind the draws) drains the queue under them.
        let ahead = ctx_field(warp, ctx, "fence-signaled").saturating_sub(dl_before);
        if met == 0 && ahead == 0 {
            break (dl_before, dl_t0, dl_c0);
        }
        let in_window = presents_served(tap).saturating_sub(settle.served);
        let _ = rb_wait_signaled(warp, ctx, dl_before, RB_DEADLINE_SUBMITS as u64 + 1, dl_t0, "deadline unconstructed");
        // `ahead` counts the readback's own fence after the queue's.
        t_putstr(&format!(
            "warp-prove: C6-RB DEADLINE attempt {} not constructed -- its sends met an engine stall ({} refusals) or {} of the {} submits had retired when the readback was taken{}, so it may have published into a draining queue; the console {}, and {} presents were served in the window\n",
            dl_try,
            met,
            ahead.min(RB_DEADLINE_SUBMITS as u64),
            RB_DEADLINE_SUBMITS,
            if ahead > RB_DEADLINE_SUBMITS as u64 { ", and the readback too" } else { "" },
            settle.said(),
            in_window
        ));
        if dl_try >= RB_DEADLINE_ATTEMPTS {
            rb_incomplete("deadline-unconstructed");
        }
    };
    let mut late: Vec<u32> = Vec::new();
    fb_state_stream(&mut late, 1);
    for _ in 0..RB_LATE_DRAWS {
        draw_stream(&mut late, 1.0, 0.0, 0.0);
    }
    submit_stream(warp, ctx, &late, "deadline later submit");
    let mut b_refused = 0u64;
    let mut b_max_ms = 0u64;
    let mut b_sum_ms = 0u64;
    let b_n = 10u64;
    // The queue's standing time is judged from the first present: when the
    // readback's retirement was first seen, sampled after each present.
    let dl_p0_ms = dl_t0.elapsed().as_millis() as u64;
    let mut dl_seen_ms: Option<u64> = None;
    for i in 0..b_n {
        for px in b.pixels().iter_mut() {
            *px = 0xFF00_0000 | (0x20 + 8 * i as u32);
        }
        let t = libthyla_rs::time::Instant::now();
        let r = b.present(None);
        let ms = t.elapsed().as_millis() as u64;
        b_sum_ms += ms;
        if ms > b_max_ms {
            b_max_ms = ms;
        }
        if r.is_err() {
            b_refused += 1;
            t_putstr(&format!("warp-prove: C6-RB DEADLINE -- B present {} refused after {} ms\n", i, ms));
        }
        if dl_seen_ms.is_none()
            && ctx_field(warp, ctx, "fence-signaled") > dl_before + RB_DEADLINE_SUBMITS as u64
        {
            dl_seen_ms = Some(dl_t0.elapsed().as_millis() as u64);
        }
    }
    // The queue, then the readback, then the later submit retire.
    let dl_rb_ms =
        rb_wait_signaled(warp, ctx, dl_before, RB_DEADLINE_SUBMITS as u64 + 1, dl_t0, "deadline");
    let dl_after_ms =
        rb_wait_signaled(warp, ctx, dl_before, RB_DEADLINE_SUBMITS as u64 + 2, dl_t0, "deadline later");
    let dl_queue_ms = dl_seen_ms.unwrap_or(dl_rb_ms);
    let dl_stood_ms = dl_queue_ms.saturating_sub(dl_p0_ms);
    let total = RB_DEADLINE_SUBMITS * RB_DRAWS_PER_SUBMIT;
    let landed_px = unsafe { core::ptr::read_volatile(va as *const u32) };
    let dl_c1 = rb_census(warp).unwrap_or_else(|| rb_incomplete("instrument:deadline-census"));
    if dl_c1.issued != dl_c0.issued {
        rb_incomplete("instrument:deadline-comp-rb-issued");
    }
    let landed_idx = rb_index_of(landed_px, total);
    // B8G8R8A8: the queue paints (R 0, G 255, B index); the later submit paints
    // R 255, G 0.
    let landed_late = (landed_px >> 16) & 0xff == 0xff;
    let landed_queue = !landed_late && (landed_px >> 8) & 0xff == 0xff;
    let cctl = open_read_all(warp, &format!("ctx/{}/ctl", ctx));
    let rb_parked = parse_field(&cctl, "readback-parked-ms");
    let rb_exec = parse_field(&cctl, "readback-exec-ms");
    // The revival is the serve loop's next pump after the retirement, so the
    // first present may still meet the stall; a second of retries bounds it.
    let mut alive = false;
    for _ in 0..20 {
        if b.present(None).is_ok() {
            alive = true;
            break;
        }
        let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(50));
    }
    let poisoned_after = parse_field(&open_read_string(warp, "ctl"), "poisoned").unwrap_or(99);
    let (rb_parked, rb_exec) = match (rb_parked, rb_exec) {
        (Some(p), Some(e)) => (p, e),
        _ => rb_incomplete("instrument:readback-timing-absent"),
    };
    // PARKED: the positive control that the rule ran, and its discriminator --
    // a readback QEMU decoded on arrival parks 0 ms and runs the whole queue.
    let parked_ok = rb_parked >= RB_PARK_FLOOR_MS && rb_exec < RB_EXEC_BOUND_MS;
    // ORDER: the readback saw everything submitted before it and nothing after.
    let order_ok = landed_queue && landed_idx >= total as i64 - 8;
    // BOUND (vote 72a): a queue standing past the bound after B's first present
    // makes a sync step stall; it is refused, and no present waits it out. A
    // queue that stood no longer judges nothing: it is not a pass.
    let stood = dl_stood_ms > RB_F2B_BOUND_MS;
    let bounded = b_max_ms < RB_F2B_BOUND_MS && b_refused > 0;
    let healthy = alive && poisoned_after == 0;
    let deadline_ok = healthy && stood && bounded && parked_ok && order_ok;
    t_putstr(&format!(
        "warp-prove: C6-RB DEADLINE {} -- PARKED {} (the readback waited {} ms in Lictor, floor {}; ran {} ms from publication, bound {}); ORDER {} (landed draw {} of {}, pixel {:#010x}{}); BOUND {} ({} B presents, {} refused while the engine stalled, longest {} ms, bound {} ms; the queue stood {} ms past the first present, {}); HEALTH {} (engine alive after: {}; poisoned {})\n",
        if !healthy || !parked_ok || !order_ok || (stood && !bounded) {
            "FAIL"
        } else if !stood {
            "UNJUDGED"
        } else {
            "PASS"
        },
        parked_ok as u32, rb_parked, RB_PARK_FLOOR_MS, rb_exec, RB_EXEC_BOUND_MS,
        order_ok as u32, landed_idx, total, landed_px,
        if landed_late {
            " -- the LATER submit's colour"
        } else if landed_px == SENTINEL {
            " -- SENTINEL: the readback wrote nothing here"
        } else {
            ""
        },
        (stood && bounded) as u32, b_n, b_refused, b_max_ms, RB_F2B_BOUND_MS, dl_stood_ms,
        if stood { "so a present must have stalled at its bound" } else { "too short to judge the bound" },
        healthy as u32, alive as u32, poisoned_after
    ));
    t_putstr(&format!(
        "warp-prove: C6-RB F2B REPORT -- bystander present latency behind a client readback of a busy BO: max {} ms mean {} ms over {} presents; the {} heavy submits + the readback retired in {} ms, the later submit in {} ms\n",
        b_max_ms, b_sum_ms / b_n, b_n, RB_DEADLINE_SUBMITS, dl_rb_ms, dl_after_ms
    ));
    if healthy && parked_ok && order_ok && !stood {
        rb_incomplete("deadline-unjudged");
    }
    record(deadline_ok, "deadline");

    // ---- ABANDON (the vote-81 round's F1): a readback parked behind chains that
    // are then abandoned is released, so its context's fences drain.
    record(rb_abandoned_prefix(tap), "abandon");

    // ---- CREATE / DESTROY: a create or a destroy a stall met owes a destroy,
    // and the slot mints again once it runs. FREE: a free a stall met is owed.
    record(rb_stalled_create(), "create");
    record(rb_stalled_destroy(), "destroy");
    record(rb_stalled_free(), "free");
    // ---- TAKEN: a free a poisoned chain stands in the way of is taken as a
    // debt and paid once the chain has retired.
    record(rb_taken_free(), "taken");
    // ---- BUSY: once its ctx is live again, a busy ctx cannot hold a taken
    // free off: a chain that can reach it parks until it is paid.
    record(rb_taken_busy(), "busy");
    // ---- YIELD: a repeat that met a stall stands the pass down for the
    // window after it, then the pass pays.
    record(rb_yield(), "yield");
    // ---- APART: another client's free standing the pass down leaves the
    // barrier on this ctx's free up. LIFT: the free's own repeat meeting a
    // stall lets the chains held back for it go to the device for the window.
    record(rb_apart(), "apart");
    record(rb_lift(), "lift");

    // ---- Teardown: withdraw the adoption, destroy the ctx, drop the
    // surfaces; the seam must be clean.
    let _ = write_ctl(warp, &format!("ctx/{}/ctl", ctx), "present-to off");
    let _ = a.surface_ctl("glsrc off");
    if !write_ctl(warp, &format!("ctx/{}/ctl", ctx), "destroy") {
        fail("readback: ctx destroy");
    }
    // Every slot back: the reserved one idle and the whole client pool free,
    // within a bound (the last retirements land at the next pump).
    let pool = parse_field(&open_read_string(warp, "ctl"), "fenced-pool").unwrap_or(0);
    let mut c_end = rb_census(warp).unwrap();
    let mut free_end = fenced_free(warp);
    for _ in 0..40 {
        if c_end.slot == 0 && free_end == pool {
            break;
        }
        let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(50));
        c_end = rb_census(warp).unwrap();
        free_end = fenced_free(warp);
    }
    let poisoned_end = parse_field(&open_read_string(warp, "ctl"), "poisoned").unwrap_or(99);
    drop(a);
    drop(b);
    unsafe {
        t_close(tap);
        t_close(warp);
    }
    if poisoned_end != 0 || c_end.slot != 0 || pool == 0 || free_end != pool {
        t_putstr(&format!(
            "warp-prove: C6-RB CLEAN FAIL -- poisoned {} rb-slot {} fenced-free {} of {} after teardown\n",
            poisoned_end, c_end.slot, free_end, pool
        ));
        record(false, "clean");
    } else {
        t_putstr(&format!(
            "warp-prove: C6-RB CLEAN PASS -- poisoned 0, rb-slot 0, fenced-free {} of {} after teardown\n",
            free_end, pool
        ));
    }
    t_putstr(&format!(
        "warp-prove: C6-RB census at end -- issued {} landed {} coalesced {} rb-abandoned {} rb-slot {}\n",
        c_end.issued, c_end.landed, c_end.coalesced, c_end.abandoned, c_end.slot
    ));
    if verdict_ok {
        t_putstr("warp-prove: C6-READBACK DONE\n");
    } else {
        rb_incomplete(first_bad.unwrap_or("unknown"));
    }
}
