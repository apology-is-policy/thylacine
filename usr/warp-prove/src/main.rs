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
            t_putstr("warp-prove: C0-REJECT DONE\n");
            return 0;
        }
        observe_rejection();
        return 0;
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
    t_putstr("WARP-PROVE PASS (ctx create/destroy + CCMD round-trip + poisoned path + two-client + corpse-reclaim + C0-P1 cross-ctx blit)\n");
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
fn warp_connect(what: &str) -> i64 {
    let fd = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv/warp".as_ptr(), 9, T_OREAD) };
    if fd < 0 {
        fail(&format!("two-client: open /srv/warp for {}", what));
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

fn ctx_field(root: i64, ctx: u32, key: &str) -> u64 {
    parse_field(&open_read_string(root, &format!("ctx/{}/ctl", ctx)), key)
        .unwrap_or_else(|| fail(&format!("two-client: ctx ctl `{}` missing (test-mode build?)", key)))
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
    let fd = unsafe {
        let p = format!("ctx/{}/submit", ctx);
        t_open(root, p.as_ptr(), p.len(), T_OWRITE)
    };
    if fd < 0 {
        fail(&format!("cross-blit: submit open ({})", what));
    }
    let n = unsafe { t_write(fd, bytes.as_ptr(), bytes.len()) };
    unsafe { t_close(fd) };
    if n != bytes.len() as i64 {
        fail(&format!("cross-blit: submit refused ({})", what));
    }
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
    let vs_bad_0 = ctx_field(bad, ctx_bad, "verify-seq");
    let vs_ok_0 = ctx_field(ok, ctx_ok, "verify-seq");
    let wrote_bad = write_ctl(bad, &format!("ctx/{}/ctl", ctx_bad), "verify");
    let wrote_ok = write_ctl(ok, &format!("ctx/{}/ctl", ctx_ok), "verify");
    let vs_bad = ctx_field(bad, ctx_bad, "verify-seq");
    let vs_ok = ctx_field(ok, ctx_ok, "verify-seq");
    let sr_bad = ctx_field(bad, ctx_bad, "stream-rejected");
    let sr_ok = ctx_field(ok, ctx_ok, "stream-rejected");
    let at_bad = ctx_field(bad, ctx_bad, "rejected-at");
    t_putstr(&format!(
        "warp-prove: C0-DETECT verify wrote(bad {} ok {}) verify-seq(bad {}->{} ok {}->{}) \
         stream-rejected(bad {} ok {}) rejected-at(bad {})\n",
        wrote_bad as u32, wrote_ok as u32, vs_bad_0, vs_bad, vs_ok_0, vs_ok, sr_bad, sr_ok, at_bad
    ));
    let _ = if vs_bad <= vs_bad_0 || vs_ok <= vs_ok_0 {
        t_putstr(
            "warp-prove: C0-DETECT INSTRUMENT -- `verify-seq` did not advance on one or both \
             ctxs, so the probe did not run and neither reading below means anything. \
             (Pre-C-0d tapestryd? the field would read 0 and never move.)\n",
        )
    } else if sr_bad == 1 && sr_ok == 0 {
        t_putstr(&format!(
            "warp-prove: C0-DETECT PASS -- the REJECTED ctx reports stream-rejected 1 \
             (at verify {}) while the healthy ctx running the SAME verb reports 0. \
             The detector discriminates; #240 is observable in-guest.\n",
            at_bad
        ))
    } else if sr_bad == 1 && sr_ok == 1 {
        t_putstr(
            "warp-prove: C0-DETECT FAIL(vacuous) -- BOTH ctxs report stream-rejected 1. \
             The detector latches on health too, so its positive reading proves nothing.\n",
        )
    } else if sr_bad == 0 && sr_ok == 0 {
        t_putstr(
            "warp-prove: C0-DETECT FAIL(blind) -- the rejected ctx reports 0 after a verify \
             that DID run. The probe's copy reached the host on a ctx vrend had latched, \
             or the seed/readback is not landing where the compare reads.\n",
        )
    } else {
        t_putstr(
            "warp-prove: C0-DETECT FAIL(inverted) -- the HEALTHY ctx reports rejected and the \
             refused one does not. The two arms are crossed.\n",
        )
    };
    // Sticky is the contract (recreate, never retry): a second verify on the
    // healthy ctx must not drift, and on the rejected one must not clear.
    let _ = write_ctl(bad, &format!("ctx/{}/ctl", ctx_bad), "verify");
    let _ = write_ctl(ok, &format!("ctx/{}/ctl", ctx_ok), "verify");
    t_putstr(&format!(
        "warp-prove: C0-DETECT re-verify stream-rejected(bad {} ok {}) -- want (1 0), sticky\n",
        ctx_field(bad, ctx_bad, "stream-rejected"),
        ctx_field(ok, ctx_ok, "stream-rejected")
    ));

    t_putstr("warp-prove: C0-REJECT DONE\n");

    unsafe {
        t_close(bad);
        t_close(ok);
    }
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
fn prove_poisoned_path(root: i64) {
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
    // One page is the minimum the seam accepts, and it is what makes the
    // COUNT cap (not the byte cap) the binding one below.
    let small = format!(
        "create3d {} {} {} 1 1 1 1 0 0 0 {}",
        PIPE_TEXTURE_2D, VIRGL_FORMAT_B8G8R8A8_UNORM, VIRGL_BIND_RENDER_TARGET, 4096
    );
    if !write_ctl(root, &format!("ctx/{}/bo/{}/ctl", ctx, bo), &small) {
        fail("poisoned-path create3d");
    }
    let submit_path = format!("ctx/{}/submit", ctx);
    let fd = unsafe { t_open(root, submit_path.as_ptr(), submit_path.len(), T_OWRITE) };
    if fd < 0 {
        fail("poisoned-path submit open");
    }
    let nop = [cmd0(VIRGL_CCMD_CLEAR, 0, 0).to_le_bytes()].concat();
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
