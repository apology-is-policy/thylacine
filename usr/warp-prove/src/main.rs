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

    unsafe { t_close(root) };
    t_putstr("WARP-PROVE PASS (ctx create/destroy + CCMD round-trip + poisoned path + two-client)\n");
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

    // --- L5: abandon is scoped to the abandoning client --------------------
    // A fresh conn, because A is gone. If `warp-abandon` were global (the
    // shape the pre-#178 P_CTL twins had) it would poison B along with A2.
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
    // refuses by attempt 17. PRE-fix nothing capped the count, so this loop
    // ran to the BYTE cap -- 64 MiB / 4 KiB = 16384 attempts -- silently
    // dropping every WarpBo past the 16-wide graveyard and leaking a kernel
    // handle plus a mapping with each one. So "refused by 17" is the
    // discriminator; "eventually refused" is true BOTH ways and would be a
    // gate that cannot fail.
    const CAP: u32 = 16; // MAX_WARP_BOS_PER_CTX
    let mut refused_at = 0u32;
    // The failure message carries the per-round (poisoned, leaked-count)
    // trace, so ONE red run says WHICH way it broke rather than only that
    // it broke: a climbing count with no refusal indicts the gate, a count
    // stuck at 0 indicts the leak accounting, and poisoned flipping to 0
    // mid-loop indicts an unexpected vindication. A bare "was never
    // refused" sends you back for a second run to learn that.
    let mut trace = String::new();
    for i in 1..=(CAP + 1) {
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
            "BO creation was never refused in CAP+1 poisoned-churn rounds -- \
             the leak count is unbounded (round-6 F1); trace(round:poisoned,leaked-count):{}",
            trace
        ));
    }
    t_putstr(&format!(
        "warp-prove: poisoned churn refused at attempt {} (cap {})\n",
        refused_at, CAP
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
