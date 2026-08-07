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
const VIRGL_CCMD_SET_SUB_CTX: u32 = 28;
const VIRGL_CCMD_CREATE_SUB_CTX: u32 = 29;
const VIRGL_OBJECT_SURFACE: u32 = 8;
// virgl_hw.h
const VIRGL_FORMAT_B8G8R8A8_UNORM: u32 = 1;
const VIRGL_BIND_RENDER_TARGET: u32 = 1 << 1;
// gallium p_defines.h values the virgl wire carries verbatim (vrend decodes
// them against the same constants; frozen with the protocol).
const PIPE_TEXTURE_2D: u32 = 2;
const PIPE_CLEAR_COLOR0: u32 = 1 << 2;

const W: u32 = 64;
const H: u32 = 64;
const BO_SIZE: u32 = W * H * 4;
const SENTINEL: u32 = 0x4141_4141;
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

fn parse_u32_prefix(s: &str) -> Option<u32> {
    s.trim().split_ascii_whitespace().next()?.parse().ok()
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
    let ctl2 = open_read_string(root, "ctl");
    match parse_field(&ctl2, "ctxs") {
        Some(0) => {}
        Some(v) => fail(&format!("ctxs {} after destroy (want 0)", v)),
        None => fail("ctl reread"),
    }

    unsafe { t_close(map_fd) };
    unsafe { t_close(root) };
    t_putstr("WARP-PROVE PASS (ctx create/destroy + CCMD round-trip)\n");
    unsafe { t_exits(0) }
}
