// The tapestryd 9P server + compositor core (Tapestry G-3; TAPESTRY.md
// section 18). Serves the /dev/tapestry tree (section 18.5, stage 0: ctl +
// surface/) over /srv + dev9p -- the ptyfs/netd native-server lineage: the
// Conn/fid table, frame extractor, dispatch, deferred replies, and the
// 4-site cancel discipline are the audited ptyfs shapes; the Tweft handler,
// the deferred-read tri-state, and the per-conn qid ownership gate are the
// audited netd shapes.
//
// THE I-40 PRESENT HALF (specs/tapestry_present.tla; the server half of the
// SPEC-TO-CODE map). The surface lifecycle is the spec's state machine:
//
//   WeaveFirst  = `create W H` on the surface ctl: t_dma_create_weave (the
//                 G-2 kernel-minted share-admissible subtype) + map + zero +
//                 RESOURCE_CREATE_2D + whole-weave ATTACH_BACKING
//                 (backed := TRUE, serverRef := TRUE). `armed` becomes real
//                 LAZILY at the first Tweft (weft_ensure below) -- the
//                 netd precedent; the Map guard is indifferent to when the
//                 registration happens, only that retire disarms it.
//   Reweave     = `resize W H <serial>` on the surface ctl (G-6b): the
//                 ack of a size-changing CONFIGURE mints the NEW weave
//                 generation (fresh DMA + fresh resource id) and is THE
//                 GENERATION FENCE -- the Rwrite completes only after the
//                 allocation (reply-after-alloc, R2-F5), and the conn
//                 stream is FIFO, so post-ack presents validate/blit
//                 against the new geometry. The displaced generation
//                 drains passively (never read again; its last content
//                 stays displayed) and retires at the first post-fence
//                 present (RetireDisplaced + ServerRelease) or with the
//                 surface. At most one drains (<=2 gens; busy -> E_AGAIN).
//   Map         = kernel-side (G-2): the client's SYS_WEFT_MAP claims the
//                 registered share consume-once. tapestryd never observes
//                 the claim; its Woven->Live edge rides the first present.
//   Submit/Complete = h_present: every present is handled SYNCHRONOUSLY --
//                 validate, TRANSFER_TO_HOST_2D, RESOURCE_FLUSH, reply
//                 Rwrite (the client's CQE = the D1 recycle gate). The
//                 in-flight window opens and closes INSIDE one dispatch, so
//                 the in-flight present set is EMPTY at every retire
//                 decision point: the tapestry_present.tla quiesce
//                 obligation (ServerRelease's "intransfer = 0") holds BY
//                 CONSTRUCTION at stage 0. A pipelined controlq (G-6+) must
//                 implement the real drain before touching retire.
//   Complete's displayed update = scanout_take(): on a present completion,
//                 a surface with no scanout owner takes scanout (the F16
//                 switch-at-first-present-COMPLETE alignment; never before
//                 its first frame has transferred).
//   Destroy/ServerRelease = retire(): ctl `destroy`, the owning conn's
//                 teardown/Tversion reset, or the R2-F4 WEDGE. Ordering
//                 (the I-40 obligation this server EXISTS to uphold):
//                 (1) quiesce -- empty by construction, asserted above;
//                 (2) SYS_WEFT_UNSHARE (registry-removal-BEFORE-page-free;
//                     discharges the spec Map guard's wstate half -- a Tweft
//                     claim racing the retire finds nothing and fails
//                     closed; on an already-claimed share the unshare is a
//                     harmless miss);
//                 (3) scanout release (SET_SCANOUT 0 if displayed);
//                 (4) DETACH_BACKING + RESOURCE_UNREF (the GPU resource
//                     dies before its backing);
//                 (5) unmap + close the weave DMA (serverRef := FALSE; the
//                     pages free when the client's mapping ref also drops,
//                     #847 -- or when the R2-F3 kernel reaper force-reclaims
//                     an orphaned client mapping after tapestryd itself
//                     dies, the ServerDeath leg).
//
// F2 (per-session isolation): a surface is resolvable ONLY by the conn that
// minted it. Every client attaches its OWN session (open=connect on
// /srv/tapestry mints a fresh SrvConn + dev9p session per opener), so
// conn == client session; walk/readdir/open/ops all gate on owner_conn +
// the per-slot generation (the netd net-3d slot-reuse discipline). Procs
// that deliberately SHARE one session (fd inheritance, or ops through the
// shared /dev/tapestry boot mount) share its surfaces -- the Plan 9
// shared-mount semantic, capability-coherent (the session IS the
// capability).
//
// F9 (caps): MAX_SURFACES_PER_CONN + the dimension bound (<= display) at
// create. R2-F4 (never-drop set): FRAME coalesces/drops; a non-droppable
// event overflowing the bounded queue WEDGES the surface (force-retire +
// CLOSE), never blocks and never drops a control event for a live client.

use alloc::collections::VecDeque;
use alloc::string::String;
use alloc::vec::Vec;

use libthyla_rs::ninep as p9;
use libthyla_rs::time::Instant;
use libthyla_rs::{
    t_burrow_detach, t_close, t_dma_create_gpu_bo, t_dma_create_weave, t_dma_map, t_srv_peer,
    t_weft_share, t_weft_unshare, TSrvPeerInfo, T_GID_SYSTEM, T_PRINCIPAL_SYSTEM,
    T_PROT_READ, T_PROT_WRITE, T_RIGHT_MAP, T_RIGHT_READ, T_RIGHT_WRITE,
    T_SRV_PEER_FLAG_CONSOLE_RENDERER,
};

/// Present-pressure window for the idle throttle (#164): two adjacent
/// buckets of this width approximate a sliding window, so `animating()`
/// is "at least PRESENT_BURST_MIN well-formed presents in the last
/// ~2x250 ms" -- sustained >= ~8 Hz. Aurora's ~2 Hz cursor blink puts at
/// most 1 present per bucket (sum <= 2, margin 2 under the threshold),
/// so a quiet console still throttles (the residual-2 idle win holds);
/// an animating client (a game at 60 fps, scrolling output, >= 8 fps
/// video) holds the clock at the ctl rate. Content below ~8 fps stays
/// throttled -- the 15 Hz idle tick displays it losslessly.
const PRESENT_BURST_WINDOW_MS: u64 = 250;
const PRESENT_BURST_MIN: u32 = 4;

use crate::chords::{ChordAction, Chords};
use crate::gpu::{FencedErr, Gpu};
use crate::pane::{self, Dir, Layout, Mode, Rect, Role};

pub const MAX_CONNS: usize = 8;
/// Of those, at most this many may be WARP conns (audit F7). Warp-2c fed
/// a second listener into the same pool, so a GPU client opening
/// `/srv/warp` eight times filled it and BOTH listeners stopped being
/// polled -- the compositor's own listener starved, and aurora (the
/// console renderer) could not reconnect. The renderer's door is never
/// closed by GPU clients now.
pub const MAX_WARP_CONNS: usize = 4;
/// Sized for GL-scale connections, not the text protocol the original 32
/// was chosen for: a warp client holds ONE OPEN FID PER LIVE BO MAPPING
/// (the map fid's clunk is what drops the mapping, I-7), so a Quake-class
/// texture set needs hundreds of live fids on one conn. At 32 the table
/// capped a GL ctx at ~28 live textures -- every later walk refused
/// E_NOMEM at Twalk, which the Mesa client saw as create3d/open failures
/// (the #198 storm: bo-peak 26, deterministic first refusal, no server-
/// side warp diagnostic, because the refusal never reached the warp
/// dispatch). ~24 B/slot inline in Conn: 512 x 8 conns ~= 100 KiB.
const MAX_FIDS: usize = 512;
pub const SRV_MSIZE: u32 = 32768;
const SRV_MSIZE_USIZE: usize = SRV_MSIZE as usize;

/// F9: the per-client surface-count cap + the global slot pool.
const MAX_SURFACES: usize = 8;
const MAX_SURFACES_PER_CONN: usize = 4;

/// Warp-2c: the GPU-seam slot pools. ONE context per client (the I-45
/// exposure bound, GPU-DESIGN section 8: no cross-context resource naming,
/// one ctx per conn); BOs bounded per ctx (each is a kernel GPU-BO mint the
/// client's shared-map budget also bounds -- this cap is the server's own
/// bookkeeping bound, not the resource authority).
const MAX_WARP_CTXS: usize = 8;
/// Lifted 16 -> 128 at Warp-3 (st/mesa alone mints ~8 hw_res before the
/// first draw; a GL app's textures are one hw_res each), then 128 -> 1024
/// at #204: GLQuake's map load holds MORE than 128 textures live at once,
/// so every create past the cap streamed GL_OUT_OF_MEMORY (#213, 1889
/// lines on the Pi) and the draws over the missing textures fed the #198
/// GL_INVALID_OPERATION storm. The per-ctx `bo-peak` census field is the
/// witness this width is sized against -- read it after a real workload
/// rather than re-guessing. The round-6 F1 cap argument is unchanged by
/// the value: the creation-time `leaked_count + live_backed` cap admits
/// at most one graveyard park per entry, and the graveyard reserves this
/// same constant at ctx mint, so "no record is ever dropped" holds by
/// construction at any width. The rows are HEAP allocations per minted
/// ctx (~160 KiB each; 1024-wide rows outgrew the daemon's main-thread
/// stack, where `Tapestryd` lives) -- a failed allocation fails the MINT
/// clean, never a later park. The BYTE bound (WARP_CTX_BACKING_MAX) is
/// the real resource authority and does not move: 1024 BOs averaging
/// 64 KiB is exactly the 64 MiB envelope. Exposed to clients as ctl
/// `bo-cap` (the prover's churn discriminator derives from it rather
/// than hardcoding).
const MAX_WARP_BOS_PER_CTX: usize = 1024;

/// The read width a client is entitled to assume covers the WHOLE per-ctx
/// `ctl` (audit F11). Not a server buffer bound -- the file is composed at
/// full length and `read_str` honours any offset; it is the CONTRACT the
/// "appended LAST" ordering discipline exists to protect, now checked
/// instead of merely asserted in a comment.
const WCTX_CTL_SNAPSHOT: usize = 255;

/// The same contract for the GLOBAL warp `ctl` (round-3 F4). Its in-tree
/// readers take a 512-byte snapshot; measured through the last fixed-size
/// line, the current prefix is ~336 at type-max, so the headroom is real
/// but unguarded until this check.
const W_CTL_SNAPSHOT: usize = 511;
/// One ctx's share of the process-wide fenced lane (round-5 F4). Half, so
/// a second client can always make progress and no single client can drive
/// every slot into the abandonment poison.
const WARP_CTX_FENCE_MAX: usize = crate::gpu::FENCED_SLOTS / 2;
const _: () = {
    // Round-6 F2: the share is a DIVISION, so it silently degenerates.
    // At FENCED_SLOTS = 1 the cap is 0 and `fences_in_flight >= 0` is
    // always true -- every submit and transfer returns E_AGAIN forever
    // and the 3D seam is dead with no build-time signal. At 3 the cap is
    // 1, which is sound but strands the prover, whose submit+transfer
    // pair must be in flight together. Only the upper bound was pinned
    // (gpu.rs asserts 2 + 2*FENCED_SLOTS <= QUEUE_SIZE); this is the
    // floor, and it names the prover as the reason.
    assert!(WARP_CTX_FENCE_MAX >= 2);
};
/// Total live BO backing per context (audit F2). The per-BO kernel
/// envelope is 64 MiB, and 16 BOs x 8 ctxs would let clients pin 8 GiB of
/// contiguous kernel memory that NOTHING charges -- graceful-OOM, but I-32
/// exists so a non-TCB Proc cannot get there, and here the allocation is
/// laundered through the TCB driver. 64 MiB/ctx (512 MiB total worst case)
/// covers a 4K RGBA render target with room over.
const WARP_CTX_BACKING_MAX: u64 = 64 * 1024 * 1024;
/// The widest plausible bytes-per-pixel for the geometry sanity check
/// (RGBA32F is 16); the host owns real format validity per section 2.1.
const WARP_BO_MAX_BPP: u64 = 16;

/// Triple buffering (D1): one weave carries three page-aligned slots.
const WEAVE_SLOTS: u32 = 3;

/// R2-F4: the bounded per-surface event queue. FRAME coalesces; a
/// non-droppable overflow wedges the surface.
const EVENT_QUEUE_CAP: usize = 128;

const PAGE: u64 = 0x1000;

// The #240 health probe's virgl vocabulary (GPU-DESIGN 4.5.4b). The seam is
// otherwise opaque to command streams -- these exist ONLY because the probe
// is the one stream the SERVER authors. virgl_protocol.h, wire-frozen.
// 17, ONE PAST VIRGL_CCMD_BLIT (16). Both values are enum ORDINALS, and the
// first cut of this constant read 96 -- a LINE NUMBER out of a grep of
// virgl_protocol.h, the identical mistake that put BLIT at 21 in warp-prove
// (21 is GET_QUERY_RESULT). warp-prove's own `VIRGL_CCMD_BLIT = 16` carries
// that scar as a comment, and 96 should have been rejected on sight for
// being nowhere near its neighbour. Derive ordinals by counting the enum,
// never by grepping for the name.
const VIRGL_CCMD_RESOURCE_COPY_REGION: u32 = 17;
/// From `#define VIRGL_CMD_RESOURCE_COPY_REGION_SIZE 13` -- a real #define
/// with a real value, unlike the opcode above.
const VIRGL_CMD_RCR_SIZE: u32 = 13;
const PIPE_TEXTURE_2D: u32 = 2;
const VIRGL_FORMAT_B8G8R8A8_UNORM: u32 = 1;
const VIRGL_BIND_RENDER_TARGET: u32 = 1 << 1;
/// What `mark` holds. Arbitrary but FIXED, and deliberately not 0 or an
/// all-ones word: a zeroed or untouched page must never read as a healthy
/// verify.
const PROBE_MARK: u32 = 0x5741_5250; // "WARP"
/// The seed XOR base. The per-verify token mixes the sequence in, so
/// "unchanged" cannot be satisfied by a value a PREVIOUS verify left.
const PROBE_TOKEN_BASE: u32 = 0x2444_3040;

/// The weave-mapping VA window in tapestryd's own AS (bump-allocated;
/// freed VAs are not reused at stage 0 -- bounded by the surface caps per
/// generation and the 47-bit user VA space; a free-list is a v1.x seam).
// 0x0240_0000 since the mouse function (its 6-BAR window ends at
// 0x0220_0000 -- the main.rs VA-layout asserts pin the whole chain).
const WEAVE_VA_BASE: u64 = 0x0240_0000;

// =============================================================================
// The qid scheme (the ptyfs/netd bit-40 template).
// =============================================================================

const P_ROOT: u64 = 0; // the attach root (qid 0 reserved for it)
const P_CTL: u64 = 1; // global ctl
const P_SURF_DIR: u64 = 2; // surface/
const P_SURF_NEW: u64 = 3; // surface/new
const P_LAYOUT: u64 = 4; // the container tree (G-6)
const P_PANE_DIR: u64 = 5; // pane/

const SURF_FLAG: u64 = 1 << 40;
const PANE_FLAG: u64 = 1 << 41; // pane qids (G-6): PANE_FLAG | id<<8 | fk
const FK_MASK: u64 = 0xff;
const N_MASK: u64 = 0x00ff_ffff;
/// The warp id field is wider than the surface/pane one (audit F11): warp
/// qids reserve bits 38+ for their level flags, so bits 8..38 are free.
/// A 30-bit field makes the monotonic-id-never-aliases property real
/// rather than "true for the first 2^24 mints" -- ctx/BO resolvers compare
/// the FULL u32 pub_id, so a truncating qid would resolve to an earlier
/// live object of the same conn once the counter passed the mask.
const WARP_N_MASK: u64 = 0x3fff_ffff;

const FK_DIR: u64 = 0;
const FK_CTL: u64 = 1;
const FK_WEAVE: u64 = 2;
const FK_PRESENT: u64 = 3;
const FK_EVENT: u64 = 4;
const FK_GEOMETRY: u64 = 5;

// Pane-file kinds (pane/<id>/*).
const PFK_DIR: u64 = 0;
const PFK_CTL: u64 = 1;
const PFK_MODE: u64 = 2;
const PFK_ROLE: u64 = 3;
const PFK_TAG: u64 = 4;
const PFK_SURFACE: u64 = 5;
const PFK_GEOMETRY: u64 = 6;

fn make_surf(n: usize, fk: u64) -> u64 {
    SURF_FLAG | ((n as u64 & N_MASK) << 8) | (fk & FK_MASK)
}
fn surf_n(path: u64) -> usize {
    ((path >> 8) & N_MASK) as usize
}
fn surf_fk(path: u64) -> u64 {
    path & FK_MASK
}
fn is_surf(path: u64) -> bool {
    path & SURF_FLAG != 0
}

/// Pane qids name the pane's PUBLIC id (monotonic, never reused -- the
/// net-3d discipline structurally: a stale pane fid resolves to nothing).
/// PIN (G-6d F4): the qid carries only the low N_MASK (24) bits of the id,
/// while the `layout` file parses the FULL u32 from its command string. The
/// two agree for the first 2^24 pane allocations; past that the pane-ctl-file
/// path (truncated qid) and the layout-file path (full id) would diverge for
/// the same pane (a miss -> E_NOENT, never a crash or a cross-pane alias --
/// ids stay unique). ~16.7M split+close cycles over the wire: unreachable.
/// Widen the pane-id field (bits 8..40 are free below PANE_FLAG) before that
/// assumption can bite.
fn make_pane(id: u32, fk: u64) -> u64 {
    PANE_FLAG | ((id as u64 & N_MASK) << 8) | (fk & FK_MASK)
}
fn pane_id(path: u64) -> u32 {
    ((path >> 8) & N_MASK) as u32
}
fn pane_fk(path: u64) -> u64 {
    path & FK_MASK
}
fn is_pane(path: u64) -> bool {
    path & PANE_FLAG != 0
}

// --- The /dev/warp tree (Warp-2c; GPU-DESIGN.md section 4.1) ----------------
//
// A SECOND service tree served by the same process (the section 5 placement:
// tapestryd hosts the GPU service on QEMU because warden binding is one
// exclusive claimant per function). Warp conns attach at W_ROOT instead of
// P_ROOT (Conn.root); the two trees share the conn/fid machinery and the
// qid space is disjoint by WARP_FLAG. Contexts and BOs carry MONOTONIC
// public ids (the pane discipline: never reused, so a stale fid resolves to
// nothing and no generation machinery is needed); the DEVICE ctx id is the
// slot index + 1 (small + reused only after a synchronous CTX_DESTROY --
// virglrenderer's context-id space is bounded, a monotonic id is not).

const WARP_FLAG: u64 = 1 << 42;
const WARP_CTX: u64 = 1 << 39; // a ctx/<id> node (below the tag bits)
const WARP_BO: u64 = 1 << 38; // a ctx bo/<id> node

const W_ROOT: u64 = WARP_FLAG;
/// The attach roots by listener (main.rs hands the accepting listener's
/// root to Conn::new).
pub const ROOT_TAPESTRY: u64 = P_ROOT;
pub const ROOT_WARP: u64 = W_ROOT;
const W_CTL: u64 = WARP_FLAG | 1;
const W_CAPS: u64 = WARP_FLAG | 2;
const W_CTX_DIR: u64 = WARP_FLAG | 3;
const W_CTX_NEW: u64 = WARP_FLAG | 4;

// Ctx-level file kinds (ctx/<id>/*).
const WFK_DIR: u64 = 0;
const WFK_CTL: u64 = 1;
const WFK_SUBMIT: u64 = 2;
const WFK_FENCE: u64 = 3;
const WFK_BO_DIR: u64 = 4;
const WFK_BO_NEW: u64 = 5;
// BO-level file kinds (ctx/<id>/bo/<id>/*), under WARP_BO.
const WFK_BO_CTL: u64 = 1;
const WFK_BO_MAP: u64 = 2;
const WFK_BO_INFO: u64 = 3;

fn make_wctx(id: u32, fk: u64) -> u64 {
    WARP_FLAG | WARP_CTX | ((id as u64 & WARP_N_MASK) << 8) | (fk & FK_MASK)
}
fn make_wbo(id: u32, fk: u64) -> u64 {
    WARP_FLAG | WARP_BO | ((id as u64 & WARP_N_MASK) << 8) | (fk & FK_MASK)
}
fn warp_id(path: u64) -> u32 {
    ((path >> 8) & WARP_N_MASK) as u32
}
fn warp_fk(path: u64) -> u64 {
    path & FK_MASK
}
fn is_warp(path: u64) -> bool {
    path & WARP_FLAG != 0
}
fn is_wctx(path: u64) -> bool {
    is_warp(path) && path & WARP_CTX != 0 && path & WARP_BO == 0
}
fn is_wbo(path: u64) -> bool {
    is_warp(path) && path & WARP_BO != 0
}

fn is_dir(path: u64) -> bool {
    path == P_ROOT
        || path == P_SURF_DIR
        || path == P_PANE_DIR
        || (is_surf(path) && surf_fk(path) == FK_DIR)
        || (is_pane(path) && pane_fk(path) == PFK_DIR)
        || path == W_ROOT
        || path == W_CTX_DIR
        || (is_wctx(path) && (warp_fk(path) == WFK_DIR || warp_fk(path) == WFK_BO_DIR))
        || (is_wbo(path) && warp_fk(path) == WFK_DIR)
}

// Mode constants (the ptyfs set).
const S_IFDIR: u32 = 0o040000;
const S_IFREG: u32 = 0o100000;
const DIR_MODE: u32 = S_IFDIR | 0o555;
const FILE_RW: u32 = S_IFREG | 0o666;
const P9_GETATTR_SIZE: u64 = 0x200;

// =============================================================================
// The tpresent descriptor (section 18.2; 32 bytes, version-pinned).
// =============================================================================

pub const TPRESENT_LEN: usize = 32;
pub const TPRESENT_V1: u32 = 1;
pub const TPRESENT_HOLD: u32 = 1 << 0; // section 18.6 determinism (G-6c)

/// One additional damage rect (multi-rect present, G-6c): rect_count k >= 2
/// rides rects 1..k INLINE after the 32-byte header (payload 32 + 16*(k-1)).
/// The as-built D4 "compositor case": under D3 the whole present payload
/// already lives in the client's registered buffer, so a separate
/// buf_idx_or_off slice reference would be redundant indirection -- the
/// inline array preserves the registered-buffer intent with zero extra
/// machinery.
pub const TRECT_LEN: usize = 16;
/// The rect-count bound (untrusted-client boundary: validation is O(k)).
pub const TPRESENT_MAX_RECTS: u32 = 64;

// =============================================================================
// The tevent record (section 18.4; 24 bytes, version-pinned wire).
// =============================================================================

pub const TEVENT_LEN: usize = 24;

pub const TEV_KEY: u16 = 1;
// Pointer kinds (G-7c; section 18.4 wire semantics): MOVE value packs the
// surface-RELATIVE x<<16|y (never absolute screen coords -- the D5 wall);
// BTN code = the evdev BTN_* button, value = press(1)/release(0); SCROLL
// value = the signed wheel delta as u32 (i32 wrap). All carry mods.
pub const TEV_PTR_MOVE: u16 = 2;
pub const TEV_PTR_BTN: u16 = 3;
/// Relative pointer motion (the mouse-look kind): value packs signed
/// display-pixel deltas dx<<16|dy (i16 each), routed to the FOCUSED
/// surface -- exact from a relative device (virtio-mouse), synthesized
/// from consecutive absolute motion (so abs-only frontends -- QEMU cocoa
/// with a tablet present never produces host rel events -- still drive
/// mouse-look). Coalesces by SUMMATION, droppable under stall.
pub const TEV_PTR_REL: u16 = 9;
pub const TEV_SCROLL: u16 = 4;
pub const TEV_FRAME: u16 = 5;
#[allow(dead_code)]
pub const TEV_CONFIGURE: u16 = 6;
#[allow(dead_code)]
pub const TEV_FOCUS: u16 = 7;
// CLOSE is the queued exit REQUEST (a compositor-initiated pane close
// strands the surface + asks the client to leave); a retired surface's
// stream-END is still the event-fid EOF (poll_events' dead-surface arm +
// h_read's gone-surface arm). Request and end are distinct on purpose.
pub const TEV_CLOSE: u16 = 8;

#[derive(Clone, Copy)]
pub struct Tevent {
    pub kind: u16,
    pub code: u16,
    pub value: u32,
    pub rune: u32,
    pub mods: u16,
    pub flags: u16,
    pub tick: u64,
}

impl Tevent {
    fn encode(&self, out: &mut [u8]) {
        out[0..2].copy_from_slice(&self.kind.to_le_bytes());
        out[2..4].copy_from_slice(&self.code.to_le_bytes());
        out[4..8].copy_from_slice(&self.value.to_le_bytes());
        out[8..12].copy_from_slice(&self.rune.to_le_bytes());
        out[12..14].copy_from_slice(&self.mods.to_le_bytes());
        out[14..16].copy_from_slice(&self.flags.to_le_bytes());
        out[16..24].copy_from_slice(&self.tick.to_le_bytes());
    }
    fn coalescible(&self) -> bool {
        // R2-F4: the droppable class is exactly {FRAME, PTR_MOVE,
        // PTR_REL} -- lossy-under-stall streams; a motion burst must
        // never WEDGE (force-retire) a slow client.
        self.kind == TEV_FRAME || self.kind == TEV_PTR_MOVE || self.kind == TEV_PTR_REL
    }
}

// =============================================================================
// The surface table (the compositor's domain state).
// =============================================================================

/// The weave backing: one G-2 share-admissible DMA chunk (server-side
/// handle + mapping + the lazily-minted share registration).
struct Weave {
    handle: i64,
    va: u64,
    size: u64,
    share_id: Option<u64>, // minted at the first Tweft (armed); idempotent
}

#[derive(PartialEq, Clone, Copy)]
enum SurfState {
    Minted, // surface id allocated; no weave yet
    Woven,  // weave + resource up; no present yet
    Live,   // presents flowing
}

struct Surface {
    gen: u32,        // the slot-reuse guard (net-3d); fids capture it at bind
    owner_conn: u64, // F2: the minting conn's id
    state: SurfState,
    w: u32,
    h: u32,
    /// The last letterbox placement logged (one-shot diagnostic).
    lb_logged: Option<(u32, u32, u32, u32)>,
    /// The present-style latch (#56): set the first time a present's
    /// damage does not cover the full surface; never cleared. A latched
    /// surface is an ACCUMULATOR (aurora's cell-diff over rotating weave
    /// slots): each slot is patchwork, so scaling any one slot composes
    /// alternating half-stale frames -- a size mismatch therefore CROPS
    /// (damage-clipped) instead of letterboxing. Full-frame presenters
    /// (the SDL class, the battery) never latch and letterbox both
    /// directions. One-way by design: a later full redraw must not flap
    /// the placement back.
    patchwork: bool,
    slot_stride: u64,
    /// The CURRENT weave generation (the spec's g-highest). weft_ensure,
    /// the geometry reads, and every post-fence present serve/validate
    /// against THIS one.
    weave: Option<Weave>,
    /// The CURRENT generation's GPU resource (per-generation ids minted
    /// from Comp.res_seq -- a reweave's fresh resource never aliases the
    /// old one or SCREEN_RES).
    resource_id: u32,
    /// The DISPLACED generation draining after a resize ack (weave + its
    /// resource id). At most one -- the spec's <=2-gens bound: a second
    /// ack while this drains is E_AGAIN (busy). Retired by the first
    /// post-fence present (RetireDisplaced + ServerRelease) or the
    /// surface retire.
    old_weave: Option<(Weave, u32)>,
    /// The CONFIGURE serial counter (section 18.3; low 16 bits ride the
    /// tevent `code`).
    cfg_serial: u16,
    /// The last CONFIGURE issued: (serial, w, h). The resize ack must
    /// echo exactly this; a newer emission overwrites it (only the
    /// latest offer is ackable -- the wayland serial dance).
    offered: Option<(u16, u32, u32)>,
    /// The client 2D resource's host-side content is stale (presents were
    /// composed, not transferred to it -- or the resource is a reweave's
    /// fresh mint). A deferred direct-scanout switch must expand its
    /// first transfer to the full surface (G-6).
    res_stale: bool,
    /// A TPRESENT_HOLD's deferred scanout push (section 18.6/F13, G-6c):
    /// the region whose device-visible flush waits for `release`. Held
    /// presents union in (most-recent bytes win where they overlap); a
    /// non-HOLD present flushes it implicitly.
    held: Option<Held>,
    title: String,
    events: VecDeque<Tevent>,
    /// Warp-4: the GL adoption's SURFACE half -- the warp ctx pub id this
    /// surface accepts as its display source (`glsrc <ctx>`). Display
    /// activates only while that ctx's own consent names this surface
    /// incarnation back (mutual adoption, `gl_adoption`); resolved fresh
    /// at every use, never cached, so either side's death is inert here.
    gl_src: Option<u32>,
    presents: u64, // diagnostic counter
}

/// The deferred flush a held present leaves behind. The pixel work
/// (transfer / blit) already ran inside the present dispatch -- ONLY the
/// device-visible step is deferred, so the tearing-freedom invariant
/// (client weave bytes read only inside the present dispatch) holds for
/// held presents too. A scanout-mode change between hold and release
/// stales the record (release drops it -- the structural repaint
/// superseded the held region).
#[derive(Clone, Copy)]
enum Held {
    /// Direct mode: the surface-space region awaiting RESOURCE_FLUSH.
    Direct(Rect),
    /// Composed mode: the SCREEN-space region awaiting transfer + flush.
    Composed(Rect),
}

fn rect_union(a: Rect, b: Rect) -> Rect {
    if a.is_empty() {
        return b;
    }
    if b.is_empty() {
        return a;
    }
    let x1 = a.x.min(b.x);
    let y1 = a.y.min(b.y);
    let x2 = (a.x + a.w).max(b.x + b.w);
    let y2 = (a.y + a.h).max(b.y + b.h);
    Rect { x: x1, y: y1, w: x2 - x1, h: y2 - y1 }
}

/// The compositor's own screen buffer (Composed mode). A WEAVE-subtype
/// DMA chunk -- the G-2 type discipline puts every RESOURCE_ATTACH_BACKING
/// scanout backing in that class (plain SYS_DMA_CREATE is the
/// virtqueue/command class, capped at KOBJ_DMA_MAX_SIZE = 1 MiB -- a
/// display buffer does not fit and does not belong). Share-admissible by
/// TYPE but never REGISTERED (t_weft_share is never called on it), so no
/// share_id exists for a client to claim -- unshared in practice. Since
/// cfg-3 the resource id is PER-GENERATION (minted from Comp.res_seq like
/// surface reweaves): a display-mode change builds a FRESH screen, binds
/// it, then frees the old (never a scanned-out dead resource); otherwise
/// held until process death (the RW-7 crash contract reclaims it).
struct Screen {
    handle: i64,
    va: u64,
    size: u64,
    res: u32,
}

/// The res_seq base: per-generation resource ids (surface weaves + the
/// screen since cfg-3) mint strictly above this -- no id ever aliases.
const SCREEN_RES: u32 = 0x40;

/// Warp-C C-2: the COMPOSITOR's own virgl context. Client warp ctxs take ids
/// `slot + 1` over `0..MAX_WARP_CTXS`, so this sits far above that range and
/// can never alias one -- a compositor ctx colliding with a client's would let
/// a client's stream author commands against the screen.
///
/// CAPABILITY-GATED (GPU-DESIGN 4.5.9). It exists only where VIRTIO_GPU_F_VIRGL
/// negotiated. Measured: the dev loop reports `virgl=0` on the default
/// `virtio-gpu-pci`, thyla-pi reports `virgl=1 capsets=2` on real V3D -- so the
/// composed GPU path is reachable ONLY on a GL host and the CPU path stays the
/// universal one. A tapestryd that assumed GL here would take the console dark
/// on the default device, which is what everything else boots under.
const COMPOSITOR_CTX: u32 = 0x100;

/// cfg-3 display-mode bounds (AURORA-CONFIG.md section 3.4): base
/// virtio-gpu reports one preferred rect, not a mode list, so `mode W H`
/// validates against sane bounds. The coarse dimension caps are a first
/// sanity gate; the LOAD-BEARING bound is the triple-buffered-surface
/// check in set_mode (F3): a fullscreen client surface is
/// WEAVE_SLOTS-buffered (W*H*4*3), so a mode whose screen fits but whose
/// fullscreen weave exceeds KOBJ_DMA_WEAVE_MAX_SIZE would let set_mode
/// succeed yet aurora's create fail -> a blank console. Bounding by the
/// surface the renderer will immediately create closes that band.
const MODE_MIN_W: u32 = 320;
const MODE_MIN_H: u32 = 200;
const MODE_MAX_W: u32 = 3840;
const MODE_MAX_H: u32 = 2160;
/// The kernel's KOBJ_DMA_WEAVE_MAX_SIZE (dma_handle.h) -- the per-weave
/// framebuffer-class cap a fullscreen surface at the new mode must fit.
const WEAVE_MAX_SIZE: u64 = 64 * 1024 * 1024;

/// What scanout 0 references (G-6). `Boot` = untouched since startup (the
/// kernel test pattern stays until a first present -- the stage-0 look);
/// `Off` = explicitly disabled after content went away.
#[derive(Clone, Copy, PartialEq)]
enum Scanout {
    Boot,
    Off,
    Direct(usize),
    Composed,
}

pub struct Comp {
    pub gpu: Gpu,
    surfaces: [Option<Surface>; MAX_SURFACES],
    gen_seq: u32,
    conn_seq: u64,
    /// The ctx-less half of the create3d refusal one-shots (WDIAG_* bits):
    /// parse/no-record/OPNOTSUPP arms fire before a ctx resolves, and the
    /// per-ctx latch cannot carry them (the #198 hunt's blind spot). The
    /// mask is comp-global (any conn can spend a bit for the daemon's
    /// life), so it is EXPOSED in the global warp ctl (`diag-noctx-arms`)
    /// -- a spent latch must at least be visible (audit F4).
    warp_diag_noctx_arms: u32,
    /// Refusals no per-ctx counter could take (the record never resolved:
    /// E_NOENT always lands here, E_INVAL/E_OPNOTSUPP when the bo id is
    /// dead). Global-ctl twin of the per-ctx `create-refused` -- without
    /// it a post-#218 retry storm against consumed records is census-
    /// invisible (audit F3, the #210 two-ledgers shape).
    warp_create_refused_noctx: u64,
    /// GPU resource ids are PER-GENERATION (a reweave mints a fresh one);
    /// pre-incremented, so the first id is SCREEN_RES + 1.
    res_seq: u32,
    /// Warp-C C-2: is COMPOSITOR_CTX live? False on a non-GL host, where the
    /// composed path stays the CPU one. Never torn down -- like `screen`, it is
    /// held for the process lifetime and reclaimed by the RW-7 crash contract.
    comp_ctx: bool,
    /// The container tree (G-6): hosting, geometry, focus.
    layout: Layout,
    screen: Option<Screen>,
    scanout: Scanout,
    /// Warp-4: the resource id the DEVICE currently scans out (0 = none/
    /// disabled). `scanout` is the mode-machine's INTENT; this is the
    /// device's truth -- they diverge across the soft-Off windows (the
    /// 1037-style defer keeps the old pixels bound until the next
    /// present-COMPLETE). The GL death-falls key on THIS: an unref of the
    /// scanned-out resource is the one order the display cannot survive,
    /// and only the device truth can name which resource that is.
    bound_res: u32,
    /// The F16 deferred direct switch: SET_SCANOUT to this surface's
    /// resource rides its next present-COMPLETE, never earlier.
    pending_direct: Option<usize>,
    /// The layout epoch the chrome (bg + borders) was last painted at.
    chrome_epoch: u64,
    /// The visible-geometry signature at the last STRUCTURAL repaint: a
    /// focus-only epoch bump redraws borders without blanking content
    /// (idle clients must not lose their pixels to a focus ring move).
    geom_sig: u64,
    /// The FRAME clock (section 18.4): a synthesized fixed-rate tick.
    pub tick: u64,
    pub clock_hz: u32,
    /// Present-pressure buckets (#164; see PRESENT_BURST_WINDOW_MS).
    /// Single-threaded like all of Comp -- written by `present()`, read
    /// by the main loop's tick-rate decision, same loop pass.
    present_bucket_start: Option<Instant>,
    present_bucket_count: u32,
    present_prev_count: u32,
    weave_va_next: u64,
    /// The surface TEV_FOCUS was last emitted for (G-6c): reconcile
    /// compares against the layout's focused surface and emits the
    /// lost/gained pair on every change.
    last_focus: Option<usize>,
    /// Keys whose PRESS was swallowed by the Super chord layer (section
    /// 18.4: reserved chords never reach a surface); their release /
    /// repeat swallow too, even if Super lifted first (no stray release
    /// reaches a client). evdev codes are < 256. INDEPENDENT of `chords`
    /// (cfg-4): the swallow-set tracks physical key state, so a live
    /// rebind never leaks a half key-pair.
    chord_down: [u64; 4],
    /// The runtime chord binding table (cfg-4): (key, shift) -> action,
    /// seeded with the stage-0 defaults, remapped by the gated `chord`
    /// ctl verb. Also holds the inter-pane `gaps` inset.
    chords: Chords,
    /// The pointer's last display position (G-7c; tablet-absolute, scaled
    /// by the input drain). Buttons/scroll route by it.
    ptr_x: u32,
    ptr_y: u32,
    /// The last ABSOLUTE motion's display position -- the base for the
    /// synthesized TEV_PTR_REL deltas. Separate from ptr_x/ptr_y so
    /// relative-device motion never poisons the abs delta base (each
    /// source's deltas are computed within its own frame); None until
    /// the first abs motion (the seed emits no delta -- the initial
    /// (0,0)->position jump is placement, not motion).
    abs_last: Option<(u32, u32)>,
    /// Section 18.6 determinism mode (dev/test builds only -- the #880
    /// strip-for-production class, enforced by the `test-mode` cargo
    /// feature at BUILD time): the FRAME clock freezes (ticks only on
    /// `tick` ctl writes) and TPRESENT_HOLD is accepted.
    #[cfg(feature = "test-mode")]
    test_mode: bool,
    /// Warp-2c: the GPU-seam context slots + the monotonic public-id
    /// sequences (the pane discipline -- tree names are never reused).
    warp_ctxs: [Option<WarpCtx>; MAX_WARP_CTXS],
    /// A slot retired while its device context may still hold live work
    /// (round-2 F8): never re-minted, because dev_ctx is derived from the
    /// slot index and reuse would alias a stale stream onto a new client.
    warp_ctx_slot_poisoned: [bool; MAX_WARP_CTXS],
    /// Backings the wedge posture refused to free, parked per ctx SLOT so
    /// the vindication that proves the device finished can free them
    /// (round-5 F1). Without this the vindication recovered the SLOT but
    /// never the PAGES, and since the ctx -- and with it `leaked_bytes` --
    /// dies at `wctx_finish`, each recovered slot handed the client a
    /// fresh cap: 64 MiB leaked per cycle, unbounded.
    ///
    /// **This graveyard can never overflow (round-6 F1).** Round 5 bounded
    /// it "by construction: a ctx holds at most MAX_WARP_BOS_PER_CTX
    /// backings at a time" -- but `bos[]` slots are REUSED, so a
    /// poisoned-yet-live ctx could mint/build/destroy in a loop and park
    /// far more than 16 over its life, bounded only by bytes. The surplus
    /// was dropped by value, and `WarpBo` has no `Drop`, so each drop
    /// leaked a kernel handle AND a mapping. The real bound is the per-ctx
    /// `leaked_count` cap enforced at BO creation, which admits at most
    /// one park per graveyard entry -- so no record is ever dropped, and
    /// the overflow flag this used to need is gone. Heap rows since #204
    /// (the 1024 lift): capacity for the full cap is `try_reserve`d at ctx
    /// MINT -- an OOM fails the mint clean -- so the parks themselves stay
    /// infallible (a push within reserved capacity never allocates). The
    /// capacity is never shrunk while the slot is poisoned; the row is
    /// drained (records freed) only at vindication.
    warp_ctx_leaked: [Vec<WarpBo>; MAX_WARP_CTXS],
    /// The probe backings a wedge refused to free, parked per ctx SLOT
    /// exactly as the row above parks BOs (#240 audit F3). Before this the
    /// probe leaked FOREVER: the wedge arm dropped the record with only a
    /// `say!`, so "the same leak posture as BOs" was false in the one
    /// respect that BOUNDS the damage -- a BO is leak-then-reclaim, the
    /// probe was leak-and-forget, and a client can drive one wedge per
    /// FENCE_ABANDON_MS against `PROC_HANDLE_MAX` in the process that IS
    /// the console.
    ///
    /// An `Option`, not a `Vec`, is the exact shape, and that is what keeps
    /// the park infallible without borrowing any of `warp_park_leaked`'s
    /// reasoning: a ctx owns at most ONE probe and DIES when it parks, so
    /// there is no cap to couple to and no capacity to reserve.
    ///
    /// The load-bearing direction is **parked => poisoned** (the park
    /// poisons; the drain precedes the un-poison), which is what makes the
    /// mint's skip-poisoned-slots sufficient. It does NOT converse (round-2
    /// F7): a slot is also poisoned with nothing parked when the ctx had no
    /// probe to begin with, and on both destroy-refused arms. So
    /// `warp_poisoned_slots` does NOT report a parked probe -- the
    /// `probe-parked`/`probe-freed` ledger is the only reporter.
    warp_ctx_leaked_probe: [Option<CtxProbe>; MAX_WARP_CTXS],
    /// The pub id whose poison condemned each slot, so a later
    /// vindication can release it (round-3 F2). 0 = none.
    warp_ctx_vindicate: [u32; MAX_WARP_CTXS],
    warp_ctx_seq: u32,
    warp_bo_seq: u32,
    /// #204 census, the global half: max `bo_backed_peak` over every ctx
    /// that ever lived -- readable AFTER a workload's ctx is gone (the
    /// per-ctx field dies with the ctx). Read via global ctl `bo-peak`.
    warp_bo_peak: u32,
    /// The global BYTES-axis twin: max `bo_bytes_peak` over every ctx that
    /// ever lived. Read via global ctl `bo-bytes-peak`.
    warp_bo_bytes_peak: u64,
    /// The probe-graveyard ledger (#240 audit F3), process-lifetime and
    /// MONOTONIC on purpose: a reclaim is provable only by an INCREASE,
    /// where a live gauge reading 0 is equally satisfied by "the park never
    /// happened" (#184). Split in two because the two claims fail
    /// independently -- parked-without-freed is exactly the leak-forever
    /// the fix closed, and it is what a regression would look like.
    warp_probe_parked: u32,
    warp_probe_freed: u32,
    /// One-shot for the F11 per-ctx-ctl width report.
    warp_ctl_wide_said: bool,
    /// The same, for the GLOBAL ctl (round-3 F4).
    warp_gctl_wide_said: bool,
    /// Every verify that reached no verdict, across all ctxs (audit F5).
    /// The one-shot `say!`s name the first of each kind; this is the RATE,
    /// and without it silencing the storm would have destroyed the only
    /// evidence that UNKNOWN is happening at all.
    warp_verify_unknown: u32,
    /// #210 custody mirror: parked reads + request-buffer residue across
    /// ALL conns, folded per tick by main's conns walk (the ctl reader's
    /// conn cannot see its siblings). fparked/rparked = pending fence /
    /// event reads held server-side; inbuf_max = the largest partial
    /// request buffered (a persistent nonzero = a parse desync); f_ctx +
    /// f_fid identify the first parked fence read.
    #[cfg(feature = "test-mode")]
    pub w210_fparked: u32,
    #[cfg(feature = "test-mode")]
    pub w210_rparked: u32,
    #[cfg(feature = "test-mode")]
    pub w210_inbuf_max: u32,
    #[cfg(feature = "test-mode")]
    pub w210_f_ctx: u32,
    #[cfg(feature = "test-mode")]
    pub w210_f_fid: u32,
}

/// A GPU-seam rendering context (Warp-2c). Owned by one conn; the DEVICE
/// ctx id is its slot + 1 (reused only after the synchronous CTX_DESTROY).
/// The #240 health probe's two server-owned resources (GPU-DESIGN 4.5.4b).
/// `mark` is painted once at ctx create and never written again; `sentinel`
/// is seeded per verify and then copied into. Both are 1x1 B8G8R8A8, so the
/// whole probe moves 4 bytes each way.
struct CtxProbe {
    mark_res: u32,
    mark_fd: i64,
    mark_va: u64,
    sent_res: u32,
    sent_fd: i64,
    sent_va: u64,
    size: u64,
}

struct WarpCtx {
    owner_conn: u64,
    pub_id: u32,
    dev_ctx: u32,
    /// The client's declared capset + ring count (`ctl` writes). Recorded
    /// at the seam from day one; the device sees them when
    /// F_CONTEXT_INIT / per-ring fencing are negotiated (Venus).
    capset: u32,
    rings: u32,
    /// Fenced-lane bookkeeping (W2d): chains submitted-not-retired for
    /// this ctx; the DENSE per-ctx completion count (#210 -- NOT the
    /// device-global fence id); the newest count the fence file has
    /// reported. signaled > reported = one unread record.
    fences_in_flight: u32,
    fence_signaled: u64,
    fence_reported: u64,
    /// #210 ledger reconciliation: every fenced write that REACHED the
    /// dispatch funnel (rx), split by outcome (minted / E_AGAIN refused /
    /// other error). The client's `issued` counts its successful fenced
    /// writes, so at quiescence issued == minted must hold; rx - minted -
    /// again - err > 0 would be an answered-without-dispatch arm.
    fenced_rx: u64,
    fenced_minted: u64,
    fenced_again: u64,
    fenced_err: u64,
    /// A fence of this ctx was ABANDONED (never retired within the
    /// driver's bound): the device may still be writing its backings, so
    /// every later retire under this ctx LEAKS rather than frees.
    fence_poisoned: bool,
    /// Bytes of backing this ctx LEAKED (retired under a poisoned fence,
    /// so the device may still DMA them). Round-2 F3: the leak used to
    /// free the `bos[]` slot, which re-armed WARP_CTX_BACKING_MAX every
    /// iteration -- a client could leak 64 MiB per cycle without bound.
    /// Leaked bytes keep counting against the cap for the ctx's life.
    leaked_bytes: u64,
    /// Backings this ctx leaked, COUNTED (round-6 F1). The byte cap alone
    /// does not bound the count -- at the minimum accepted size (PAGE) a
    /// ctx can leak 16384 backings inside its 64 MiB budget, which is what
    /// overran the 16-wide graveyard. Capped at MAX_WARP_BOS_PER_CTX at BO
    /// creation so the graveyard is always wide enough to take every one.
    /// Both counters reset together at the live un-poison, where the pages
    /// are genuinely freed -- an uncharge is only honest when paired with
    /// the drop that FREES.
    leaked_count: u32,
    /// Destroy was requested while fences were still in flight (audit
    /// F5): the ctx is hidden from every client resolve immediately and
    /// the serve-loop pump finishes the retire once quiesced. The old
    /// shape blocked the dispatch for up to 2 s per object -- client-
    /// multipliable into minutes of frozen console (#31/#125).
    retiring: bool,
    /// #240 health probe (GPU-DESIGN 4.5.4b). Two 1x1 resources the client
    /// can never name: `mark` holds a fixed value, `sentinel` is the
    /// target a verify copies into. Kept OUT of `bos[]` deliberately --
    /// every client-facing resolve walks that array, so membership would
    /// be the reachability the audit forbids (a client that can write
    /// either can forge health, or manufacture a rejection against a
    /// healthy ctx).
    probe: Option<CtxProbe>,
    /// vrend has latched this context's command stream off: submits are
    /// still ACCEPTED and their fences still retire, but no command runs
    /// (#240, measured). STICKY, exactly like the vrend latch it mirrors,
    /// and exactly like `glGetGraphicsResetStatus` / VK_ERROR_DEVICE_LOST
    /// -- the remedy is recreate, never retry. DISTINCT from
    /// `fence_poisoned`, whose cause is a chain that never retired; #240
    /// happened because the one was read through the other.
    stream_rejected: bool,
    /// The verify sequence that caught the loss. The offending stream lies
    /// in (previous_verify, rejected_at] -- a window, never a command.
    rejected_at: u64,
    /// Verifies ADMITTED on this ctx (incremented before any device I/O),
    /// so `rejected_at` has a stable name to point at.
    verify_seq: u64,
    /// Verifies that reached a HEALTHY verdict. AUDIT F2: `verify_seq`
    /// alone cannot carry the answer, because the probe is three-valued
    /// (healthy / latched / unknown) while the ctl was two-valued -- every
    /// UNKNOWN arm left `verify_seq` advanced and `stream_rejected` 0,
    /// which the docs told readers to interpret as "asked and healthy".
    /// That is what made F1 exploitable: a blinded probe is permanently
    /// UNKNOWN, and UNKNOWN was indistinguishable from health. A reader
    /// now needs `verify_ok` to MOVE before believing a 0.
    verify_ok: u64,
    /// The compositor tick the last verify ran on. SELF-AUDIT FINDING: the
    /// `verify` verb costs THREE synchronous device round trips on the
    /// compositor's own dispatch thread, and it is client-triggered -- so
    /// unrated it is a fresh DoS lever, a client spinning writes to starve
    /// the present path. The fenced lane has `E_AGAIN` admission for exactly
    /// this reason and the sync slot bypasses all of it. One probe per ctx
    /// per tick is the bound: it is precisely the per-frame cadence 4.5.4b
    /// designs for, so it costs the intended use nothing, and it caps the
    /// whole box at MAX_WARP_CTXS probes per frame no matter what clients do.
    verify_tick: u64,
    /// One-shot latch per UNKNOWN arm (audit F5), the `build_diag_arms`
    /// shape. Both unknown arms used to `say!` unconditionally, and after
    /// F1 EVERY verify on a blinded ctx took one -- 8 ctxs at the per-frame
    /// cadence this verb is designed for is ~480 synchronous console lines
    /// a second, emitted on the compositor's own dispatch thread. The FIRST
    /// of each kind per ctx still names itself; the RATE is carried by the
    /// comp-global `verify-unknown` count instead, so silencing the storm
    /// costs no information (#95: latch the REPORT, never the counting).
    verify_diag_arms: u32,
    /// The last VERDICT this tick's probe reached, `None` for unknown
    /// (round-2 F2). The rate limit answers from this rather than from
    /// `!stream_rejected`, which cannot represent "asked, no answer".
    verify_last: Option<bool>,
    /// Warp-4: the GL adoption's CTX half -- `present-to <surface> <bo>`:
    /// this ctx consents to displaying its BO `bo_pub` on surface
    /// (slot, gen). The gen pin makes a consent die with the surface
    /// incarnation it named -- slot reuse cannot re-arm it against a
    /// future tenant.
    present_to: Option<(usize, u32, u32)>,
    /// #204 census: the most BACKED BOs this ctx ever held at once -- the
    /// quantity the creation-time cap gates, so it is the number the cap
    /// width must be sized against. Read via ctl `bo-peak`.
    bo_backed_peak: u32,
    /// #204 census, the BYTES axis: the largest live-backing byte sum this
    /// ctx ever held -- the quantity WARP_CTX_BACKING_MAX gates (bo-peak
    /// 26 with thousands of refusals showed the BYTE cap can saturate at
    /// tiny counts: few-but-large backings). Read via ctl `bo-bytes-peak`.
    bo_bytes_peak: u64,
    /// #218 one-shot diagnostic latch, PER ARM (a bitmask indexed by
    /// WDIAG_*): the FIRST refusal of each kind per ctx says which arm
    /// and with what parameters (four census runs died to silence here,
    /// and the per-CTX-once predecessor could name only one kind per ctx
    /// lifetime -- the #198 hunt needed the second). One-shot per arm so
    /// a per-texture failure loop cannot become its own console storm.
    build_diag_arms: u32,
    /// Every create3d refusal of this ctx, ALL families (counted at the
    /// ctl chokepoint beside the #218 unmint). The one-shots name the
    /// first of each kind; this counts the storm, census-readably.
    create_refused: u64,
    /// Heap row (#204): MAX_WARP_BOS_PER_CTX slots, allocated at mint.
    bos: alloc::boxed::Box<[Option<WarpBo>]>,
}

/// A GPU buffer object: a kernel-minted GPU-BO DMA chunk attached as the
/// backing of a device-global 3D resource, shared to the client by Tweft.
struct WarpBo {
    pub_id: u32,
    res_id: u32,
    dma_fd: i64,
    va: u64,
    pa: u64,
    size: u64,
    /// The lazy Tweft mint (the weft_ensure precedent); disarmed at retire
    /// BEFORE any backing free (the R2-F5 ordering).
    share_id: Option<u64>,
    w: u32,
    h: u32,
    /// Destroy requested under in-flight fences (audit F5): already
    /// unresolvable to the client; the pump frees it when the ctx
    /// quiesces (or leaks it if a fence was abandoned).
    retiring: bool,
}

/// Warp-4: a resolved ACTIVE GL adoption (see `gl_adoption`) -- the
/// device ctx + 3D resource + tapestryd's own mapping of its backing.
/// A value is valid for the single dispatch that resolved it.
#[derive(Clone, Copy)]
struct GlAdopt {
    dev_ctx: u32,
    res_id: u32,
    va: u64,
    w: u32,
    h: u32,
}

const NO_SURFACE: Option<Surface> = None;

impl Comp {
    pub fn new(gpu: Gpu) -> Comp {
        Comp {
            gpu,
            surfaces: [NO_SURFACE; MAX_SURFACES],
            gen_seq: 0,
            conn_seq: 0,
            warp_diag_noctx_arms: 0,
            warp_create_refused_noctx: 0,
            res_seq: SCREEN_RES,
            comp_ctx: false,
            layout: Layout::new(),
            screen: None,
            scanout: Scanout::Boot,
            bound_res: 0,
            pending_direct: None,
            chrome_epoch: 0,
            geom_sig: 0,
            tick: 0,
            clock_hz: 60,
            present_bucket_start: None,
            present_bucket_count: 0,
            present_prev_count: 0,
            weave_va_next: WEAVE_VA_BASE,
            last_focus: None,
            chord_down: [0; 4],
            chords: Chords::new(),
            ptr_x: 0,
            abs_last: None,
            ptr_y: 0,
            #[cfg(feature = "test-mode")]
            test_mode: false,
            warp_ctxs: [WARP_NO_CTX; MAX_WARP_CTXS],
            warp_ctx_slot_poisoned: [false; MAX_WARP_CTXS],
            warp_ctx_leaked: core::array::from_fn(|_| Vec::new()),
            warp_ctx_leaked_probe: core::array::from_fn(|_| None),
            warp_ctx_vindicate: [0; MAX_WARP_CTXS],
            warp_ctx_seq: 0,
            warp_bo_seq: 0,
            warp_bo_peak: 0,
            warp_bo_bytes_peak: 0,
            warp_probe_parked: 0,
            warp_probe_freed: 0,
            warp_ctl_wide_said: false,
            warp_gctl_wide_said: false,
            warp_verify_unknown: 0,
            #[cfg(feature = "test-mode")]
            w210_fparked: 0,
            #[cfg(feature = "test-mode")]
            w210_rparked: 0,
            #[cfg(feature = "test-mode")]
            w210_inbuf_max: 0,
            #[cfg(feature = "test-mode")]
            w210_f_ctx: 0,
            #[cfg(feature = "test-mode")]
            w210_f_fid: 0,
        }
    }

    /// Scanout-state name for the transition diagnostics (rare-path only).
    fn scanout_name(&self) -> &'static str {
        match self.scanout {
            Scanout::Boot => "boot",
            Scanout::Off => "off",
            Scanout::Direct(_) => "direct",
            Scanout::Composed => "composed",
        }
    }

    /// True while the FRAME clock is frozen (test-mode on); the serve
    /// loop skips the wall-clock tick and `tick` ctl writes drive time.
    pub fn test_frozen(&self) -> bool {
        #[cfg(feature = "test-mode")]
        {
            self.test_mode
        }
        #[cfg(not(feature = "test-mode"))]
        {
            false
        }
    }

    pub fn next_conn_id(&mut self) -> u64 {
        self.conn_seq = self.conn_seq.wrapping_add(1);
        self.conn_seq
    }

    fn surf(&self, n: usize) -> Option<&Surface> {
        self.surfaces.get(n).and_then(|s| s.as_ref())
    }
    fn surf_mut(&mut self, n: usize) -> Option<&mut Surface> {
        self.surfaces.get_mut(n).and_then(|s| s.as_mut())
    }

    /// The F2 ownership + generation gate every surface-qid consumer runs.
    fn surf_owned(&self, n: usize, conn_id: u64, gen: u32) -> bool {
        match self.surf(n) {
            Some(s) => s.owner_conn == conn_id && s.gen == gen,
            None => false,
        }
    }

    fn owned_count(&self, conn_id: u64) -> usize {
        self.surfaces
            .iter()
            .filter(|s| s.as_ref().map_or(false, |s| s.owner_conn == conn_id))
            .count()
    }

    /// Mint a surface slot for `conn_id` (F9 caps enforced by the caller).
    fn mint(&mut self, conn_id: u64) -> Option<usize> {
        let n = self.surfaces.iter().position(|s| s.is_none())?;
        self.gen_seq = self.gen_seq.wrapping_add(1);
        self.surfaces[n] = Some(Surface {
            gen: self.gen_seq,
            owner_conn: conn_id,
            state: SurfState::Minted,
            w: 0,
            h: 0,
            lb_logged: None,
            patchwork: false,
            slot_stride: 0,
            weave: None,
            resource_id: 0, // minted with the first generation (create)
            old_weave: None,
            res_stale: false,
            held: None,
            cfg_serial: 0,
            offered: None,
            title: String::new(),
            events: VecDeque::new(),
            gl_src: None,
            presents: 0,
        });
        Some(n)
    }

    fn next_res_id(&mut self) -> u32 {
        self.res_seq = self.res_seq.wrapping_add(1);
        self.res_seq
    }

    /// Allocate one weave GENERATION: DMA chunk + map + zero + a fresh 2D
    /// resource with the whole weave attached as backing. The shared body
    /// of the spec's WeaveFirst (create) and Reweave (resize ack).
    /// Returns (weave, slot_stride, resource_id); every failure path
    /// rolls back fully.
    fn alloc_weave(&mut self, w: u32, h: u32) -> Result<(Weave, u64, u32), u32> {
        let stride = (w as u64) * 4;
        let slot_bytes = stride * (h as u64);
        let slot_stride = (slot_bytes + PAGE - 1) & !(PAGE - 1);
        let size = slot_stride * (WEAVE_SLOTS as u64);

        // The G-2 mint: the kernel-tracked share-admissible weave subtype
        // (device-passive pixels; a plain SYS_DMA_CREATE region would be
        // structurally unshareable, R2-F1).
        let handle =
            unsafe { t_dma_create_weave(size, T_RIGHT_READ | T_RIGHT_WRITE | T_RIGHT_MAP) };
        if handle < 0 {
            say!("tapestryd: t_dma_create_weave({}) failed {}", size, handle);
            return Err(p9::E_NOMEM);
        }
        let va = self.weave_va_next;
        self.weave_va_next += (size + PAGE - 1) & !(PAGE - 1);
        let pa = unsafe { t_dma_map(handle, va, T_PROT_READ | T_PROT_WRITE) };
        if pa < 0 {
            unsafe { t_close(handle) };
            return Err(p9::E_NOMEM);
        }
        // Zero the weave: DMA chunk content must never leak a prior
        // occupant's bytes into a client mapping.
        unsafe { core::ptr::write_bytes(va as *mut u8, 0, size as usize) };

        let res = self.next_res_id();
        if self.gpu.resource_create_2d(res, w, h).is_err() {
            unsafe { t_burrow_detach(va, size) };
            unsafe { t_close(handle) };
            return Err(p9::E_NOMEM);
        }
        if self.gpu.attach_backing(res, pa as u64, size as u32).is_err() {
            let _ = self.gpu.resource_unref(res);
            unsafe { t_burrow_detach(va, size) };
            unsafe { t_close(handle) };
            return Err(p9::E_NOMEM);
        }
        Ok((
            Weave {
                handle,
                va,
                size,
                share_id: None,
            },
            slot_stride,
            res,
        ))
    }

    /// Tear down one weave generation's server side, in the R2-F5 order:
    /// unshare (registry-removal-before-page-free) -> the GPU resource
    /// dies before its backing -> unmap + close (serverRef -> FALSE; #847
    /// keeps the pages until the client's mapping ref drops too). The
    /// caller has already ensured no scanout references `res` (the mode
    /// machine + force-away in retire; the present-tail old drop runs
    /// after the current generation's content took the display).
    fn release_gen(&mut self, w: &Weave, res: u32) {
        if let Some(id) = w.share_id {
            let rc = unsafe { t_weft_unshare(id) };
            if rc < 0 {
                // Already claimed (consumed at Map) -- expected.
            }
        }
        let _ = self.gpu.detach_backing(res);
        let _ = self.gpu.resource_unref(res);
        unsafe { t_burrow_detach(w.va, w.size) };
        unsafe { t_close(w.handle) };
    }

    /// `create W H`: the spec's WeaveFirst -- allocate + zero the weave,
    /// create the 2D resource, attach the whole weave as its backing.
    fn create(&mut self, n: usize, w: u32, h: u32) -> Result<(), u32> {
        let (disp_w, disp_h) = (self.gpu.width, self.gpu.height);
        let s = self.surf(n).ok_or(p9::E_BADF)?;
        if s.state != SurfState::Minted {
            return Err(p9::E_EXIST); // create is once per surface
        }
        // F9: the dimension bound (a weave is tapestryd's DMA allocation).
        if w == 0 || h == 0 || w > disp_w || h > disp_h {
            return Err(p9::E_INVAL);
        }
        let (weave, slot_stride, res) = self.alloc_weave(w, h)?;

        let s = self.surf_mut(n).unwrap();
        s.w = w;
        s.h = h;
        s.slot_stride = slot_stride;
        s.weave = Some(weave);
        s.resource_id = res;
        s.state = SurfState::Woven;
        // G-6: host at create -- the focused empty leaf takes it, else the
        // focused leaf splits. A pane-table-exhausted surface stays
        // unhosted (invisible; presents complete without pixels). Hosting
        // is structural: a zoomed layout restores first (the tmux rule).
        self.layout.unzoom();
        if self.layout.host(n).is_none() {
            say!("tapestryd: surface {} unhosted (pane table full)", n);
        }
        self.reconcile();
        Ok(())
    }

    /// The Tweft mint (the netd weft_ensure precedent): register the weave
    /// once, echo the stored id thereafter. `armed` becomes TRUE here; the
    /// kernel's claim consumes it (Map); retire disarms it (unshare).
    fn weft_ensure(&mut self, n: usize) -> Option<(u64, u32)> {
        let s = self.surf_mut(n)?;
        let w = s.weave.as_mut()?;
        if let Some(id) = w.share_id {
            return Some((id, w.size as u32));
        }
        let id = unsafe { t_weft_share(w.va, w.size) };
        if id <= 0 {
            say!("tapestryd: t_weft_share failed {}", id);
            return None;
        }
        w.share_id = Some(id as u64);
        Some((id as u64, w.size as u32))
    }

    /// The resize ack `resize W H <serial>` (section 18.3; the spec's
    /// Reweave). The ack is THE GENERATION FENCE: its Rwrite completes
    /// only after the new generation is fully allocated (the R2-F5
    /// reply-after-alloc precedent), and the conn stream is FIFO, so
    /// every present the client sends after reading that Rwrite
    /// validates + blits against the NEW geometry. The displaced
    /// generation drains passively (its last content stays displayed;
    /// never read again -- tearing-freedom holds) until the first
    /// post-fence present retires it.
    ///
    /// Errors: stale serial (a newer CONFIGURE superseded) -> E_AGAIN,
    /// re-ack after draining events; unknown serial / echo mismatch ->
    /// E_INVAL; prior reweave still draining -> E_AGAIN (the <=2-gens
    /// bound: present a frame, then re-ack).
    fn resize_ack(&mut self, n: usize, w: u32, h: u32, serial: u16) -> Result<(), u32> {
        let s = self.surf(n).ok_or(p9::E_BADF)?;
        if s.weave.is_none() {
            return Err(p9::E_INVAL); // no generation to reweave
        }
        let (os, ow, oh) = s.offered.ok_or(p9::E_INVAL)?;
        if serial != os {
            // u16 compare; serial spaces are tiny per surface lifetime.
            // A wrap-straddling stale reads as "unknown" -- fail-closed
            // either way (both are rejections).
            return Err(if serial < s.cfg_serial { E_AGAIN } else { p9::E_INVAL });
        }
        if w != ow || h != oh {
            return Err(p9::E_INVAL); // the ack must echo the offer
        }
        if w == s.w && h == s.h {
            // A same-size offer (the redraw request) acked: legal no-op.
            self.surf_mut(n).unwrap().offered = None;
            return Ok(());
        }
        if s.old_weave.is_some() {
            return Err(E_AGAIN); // one reweave in flight (<=2 gens)
        }

        // Reweave: mint the new generation FIRST (a failure leaves the
        // current one untouched and the offer standing for a retry).
        let (weave, slot_stride, res) = self.alloc_weave(w, h)?;
        let s = self.surf_mut(n).unwrap();
        let old = s.weave.take().unwrap();
        let old_res = s.resource_id;
        s.old_weave = Some((old, old_res));
        s.weave = Some(weave);
        s.resource_id = res;
        s.w = w;
        s.h = h;
        s.slot_stride = slot_stride;
        s.res_stale = true; // the fresh resource has no content yet
        s.offered = None;
        // Defensive (mode-machine-unreachable: Direct(n) implies the
        // surface was display-sized, which implies any outstanding offer
        // was same-size -- handled above): if scanout still names n via
        // the OLD resource, defer to the new generation's first present.
        // No set_scanout(0) -- that would blank the user's screen
        // mid-resize; the old pixels persist until the F16 flip.
        if self.scanout == Scanout::Direct(n) {
            self.scanout = Scanout::Off;
            self.pending_direct = Some(n);
        }
        // The new size feeds the scanout-mode predicate (a letterboxed
        // single leaf acking up to display size becomes Direct-eligible).
        self.reconcile();
        Ok(())
    }

    /// Warp-C C-2: mint the compositor's own virgl context, once, if the host
    /// has GL. Returns whether it is live.
    ///
    /// Deliberately NOT fatal when absent or when CTX_CREATE fails: 4.5.9 makes
    /// the CPU-composed path permanent, so "no compositor context" is a normal
    /// operating state (every non-GL host, and bare metal, where virgl is a
    /// virtualization transport with nothing to negotiate) rather than an error
    /// to report. The one thing it must never do is leave `comp_ctx` true on a
    /// failed create -- a later blit against a context the host never built is
    /// the failure this whole arc is trying to make impossible to reach
    /// silently, since a refused stream reports SUCCESS (4.5.4a).
    /// Report the composed-path posture once at startup, and mint the context
    /// if the host has GL.
    ///
    /// This is deliberately NOT hung off the composed-scanout path where the
    /// other display resources are built. It was, and the line never printed:
    /// `ensure_screen` runs only under `Scanout::Composed`, a state a normal
    /// boot does not enter, so the report sat behind an unconstructed state and
    /// said nothing on the one host whose posture most needed saying. Which
    /// composition path is AVAILABLE is a property of the host, fixed at
    /// feature negotiation -- so it is reported where the host is brought up.
    pub fn report_composed_posture(&mut self) {
        if self.ensure_comp_ctx() {
            say!("tapestryd: composed path = GPU (compositor ctx {})", COMPOSITOR_CTX);
        } else {
            say!("tapestryd: composed path = CPU (virgl={})", self.gpu.virgl as u32);
        }
    }

    fn ensure_comp_ctx(&mut self) -> bool {
        if self.comp_ctx {
            return true;
        }
        if !self.gpu.virgl {
            return false;
        }
        if self.gpu.ctx_create(COMPOSITOR_CTX, b"tapestry").is_err() {
            say!("tapestryd: compositor ctx create failed -- staying on the CPU composed path");
            return false;
        }
        self.comp_ctx = true;
        say!("tapestryd: compositor ctx {} up (virgl)", COMPOSITOR_CTX);
        true
    }

    /// Allocate the compositor's screen buffer + resource (lazy; replaced
    /// only by a display-mode change, else kept for the process lifetime).
    fn ensure_screen(&mut self) -> bool {
        if self.screen.is_some() {
            return true;
        }
        let (dw, dh) = (self.gpu.width, self.gpu.height);
        match self.alloc_screen(dw, dh) {
            Some(s) => {
                self.screen = Some(s);
                true
            }
            None => false,
        }
    }

    /// Build one screen GENERATION at the given geometry: DMA weave chunk
    /// + map + zero + a fresh per-generation 2D resource with the chunk
    /// attached as backing. Every failure path rolls back fully; the
    /// caller's current screen is untouched.
    fn alloc_screen(&mut self, dw: u32, dh: u32) -> Option<Screen> {
        let size = ((dw as u64) * (dh as u64) * 4 + PAGE - 1) & !(PAGE - 1);
        let handle =
            unsafe { t_dma_create_weave(size, T_RIGHT_READ | T_RIGHT_WRITE | T_RIGHT_MAP) };
        if handle < 0 {
            say!("tapestryd: screen t_dma_create_weave({}) failed {}", size, handle);
            return None;
        }
        let va = self.weave_va_next;
        self.weave_va_next += size;
        let pa = unsafe { t_dma_map(handle, va, T_PROT_READ | T_PROT_WRITE) };
        if pa < 0 {
            unsafe { t_close(handle) };
            return None;
        }
        // Zero: the buffer scans out before the first chrome paint on a
        // mode change -- never a prior occupant's bytes.
        unsafe { core::ptr::write_bytes(va as *mut u8, 0, size as usize) };
        let res = self.next_res_id();
        if self.gpu.resource_create_2d(res, dw, dh).is_err() {
            unsafe { t_burrow_detach(va, size) };
            unsafe { t_close(handle) };
            return None;
        }
        if self.gpu.attach_backing(res, pa as u64, size as u32).is_err() {
            let _ = self.gpu.resource_unref(res);
            unsafe { t_burrow_detach(va, size) };
            unsafe { t_close(handle) };
            return None;
        }
        Some(Screen { handle, va, size, res })
    }

    /// Tear down a displaced screen generation (the release_gen order,
    /// minus unshare -- the screen is never registered): resource dies
    /// before its backing -> unmap + close. The caller has already
    /// ensured no scanout references `s.res` (set_mode rebinds a live
    /// Composed scanout to the NEW screen first; Direct/Boot/Off never
    /// referenced it).
    fn free_screen(&mut self, s: Screen) {
        let _ = self.gpu.detach_backing(s.res);
        let _ = self.gpu.resource_unref(s.res);
        unsafe { t_burrow_detach(s.va, s.size) };
        unsafe { t_close(s.handle) };
    }

    /// cfg-3: the display-mode change (AURORA-CONFIG.md section 3.4) --
    /// the gated `mode W H` verb's engine. Build the NEW screen first
    /// (fallible; the old survives any failure), drop stale holds, swap,
    /// rebind a live Composed scanout BEFORE freeing the old resource,
    /// then let the audited reconcile() do the rest: layout recompute at
    /// the new geometry, structural chrome repaint + flush (the #57
    /// post-bind flush lives there), the CONFIGURE fan to every visible
    /// surface, and the Direct->Composed fall (a direct surface is no
    /// longer display-sized). Boot stays Boot (pre-first-content -- the
    /// aurora startup push lands here; the surface then creates at the
    /// new geometry).
    fn set_mode(&mut self, w: u32, h: u32) -> Result<(), u32> {
        if !(MODE_MIN_W..=MODE_MAX_W).contains(&w) || !(MODE_MIN_H..=MODE_MAX_H).contains(&h) {
            return Err(p9::E_INVAL);
        }
        // F3: reject a mode whose fullscreen TRIPLE-buffered surface would
        // exceed the per-weave cap -- else set_mode would succeed but the
        // renderer's immediate fullscreen create fails, blanking the
        // console. The page-rounded slot stride matches alloc_weave.
        let slot = (((w as u64) * 4 * (h as u64)) + PAGE - 1) & !(PAGE - 1);
        let surf_size = slot * (WEAVE_SLOTS as u64);
        if surf_size > WEAVE_MAX_SIZE {
            return Err(p9::E_INVAL);
        }
        if w == self.gpu.width && h == self.gpu.height {
            return Ok(()); // same mode: a push of the current value no-ops
        }
        // F3-follow-up (the max-resolution display-brick, reported 2026-07-23):
        // the STATIC cap is not enough -- the kernel's contiguous-DMA
        // allocator can fail BELOW KOBJ_DMA_WEAVE_MAX_SIZE (buddy max-order /
        // fragmentation / physical RAM). A 2560x1440 surface (44 MiB, < the
        // 64 MiB cap) allocated its single-buffered SCREEN fine but its
        // triple-buffered SURFACE weave then failed -1 -> set_mode had
        // committed the geometry, so aurora's reweave died -> retire ->
        // the display disconnected (and, persisted, bricked every boot).
        // PRE-FLIGHT the real fullscreen surface allocation and reject the
        // mode if it cannot back a surface -- the current working geometry
        // stands, the OSD apply is refused (never persisted), and a
        // startup push of a too-big persisted mode is rejected so aurora
        // comes up at the default (the self-heal in aurora clears it).
        let probe = unsafe {
            t_dma_create_weave(surf_size, T_RIGHT_READ | T_RIGHT_WRITE | T_RIGHT_MAP)
        };
        if probe < 0 {
            say!("tapestryd: mode {}x{} refused -- surface weave {} unallocatable ({})",
                 w, h, surf_size, probe);
            return Err(p9::E_NOMEM);
        }
        unsafe { t_close(probe) };
        say!(
            "tapestryd: mode {}x{} -> {}x{} (scanout {})",
            self.gpu.width,
            self.gpu.height,
            w,
            h,
            self.scanout_name()
        );
        let new = match self.alloc_screen(w, h) {
            Some(s) => s,
            None => return Err(p9::E_NOMEM),
        };
        // Held pushes reference the OLD geometry/screen; a deferred push
        // against the new one would land wrong bytes at wrong rects. The
        // CONFIGURE fan below makes every client repaint.
        for n in 0..MAX_SURFACES {
            if let Some(s) = self.surf_mut(n) {
                s.held = None;
            }
        }
        // F2: for a DISPLAYED (Composed) scanout, rebind to the new screen
        // BEFORE committing any state -- and roll back cleanly on failure.
        // A discarded set_scanout result could leave the device scanning
        // the old resource while we free it (device DMA-scanning
        // returned-to-pool pages -- a display-integrity glitch). `new` is
        // still a local here, so a failed rebind frees it and leaves the
        // old screen + geometry byte-for-byte intact.
        if self.scanout == Scanout::Composed {
            if self.gpu.set_scanout(new.res, w, h).is_err() {
                say!("tapestryd: mode rebind failed; old geometry retained");
                self.free_screen(new);
                return Err(E_IO);
            }
            // The device now scans new.res (zeroed; reconcile's structural
            // repaint fills it this dispatch); old.res is no longer bound.
            self.bound_res = new.res;
        }
        // Commit: the old screen is now un-scanned (rebound above) or was
        // never bound (Boot/Off/Direct) -- either way safe to free.
        let old = self.screen.take();
        self.screen = Some(new);
        self.gpu.width = w;
        self.gpu.height = h;
        if let Some(o) = old {
            self.free_screen(o);
        }
        self.reconcile();
        Ok(())
    }

    /// Paint the full chrome into the screen buffer: background everywhere
    /// (blanking pane content -- panes heal on their next present) + the
    /// border frames. Client pixels enter the screen buffer ONLY inside a
    /// present dispatch (the G-6 tearing-freedom invariant), never here.
    fn paint_chrome(&mut self) {
        let (dw, dh) = (self.gpu.width as u64, self.gpu.height as u64);
        let va = match &self.screen {
            Some(s) => s.va,
            None => return,
        };
        let px = va as *mut u32;
        // SAFETY: the screen buffer is dw*dh*4 bytes, mapped RW for the
        // process lifetime.
        unsafe {
            for i in 0..(dw * dh) as usize {
                *px.add(i) = pane::BG_COLOR;
            }
        }
        self.paint_borders();
        self.paint_strips();
        self.chrome_epoch = self.layout.epoch;
    }

    /// Redraw ONLY the 1px leaf frames (focus ring moves must not blank
    /// idle clients' content).
    fn paint_borders(&mut self) {
        let dw = self.gpu.width as u64;
        let va = match &self.screen {
            Some(s) => s.va,
            None => return,
        };
        let px = va as *mut u32;
        let focused = self.layout.focused;
        for (slot, _id) in self.layout.live_ids() {
            let p = match self.layout.get(slot) {
                Some(p) => p,
                None => continue,
            };
            if !p.visible || !self.layout.is_leaf(slot) || p.rect == p.content {
                continue;
            }
            let color = if slot == focused {
                pane::FOCUS_COLOR
            } else {
                pane::BORDER_COLOR
            };
            let r = p.rect;
            // SAFETY: the geometry pass bounds every visible rect inside
            // the display; the buffer covers the display.
            unsafe {
                for x in r.x..r.x + r.w {
                    *px.add((r.y as u64 * dw + x as u64) as usize) = color;
                    *px.add(((r.y + r.h - 1) as u64 * dw + x as u64) as usize) = color;
                }
                for y in r.y..r.y + r.h {
                    *px.add((y as u64 * dw + r.x as u64) as usize) = color;
                    *px.add((y as u64 * dw + (r.x + r.w - 1) as u64) as usize) = color;
                }
            }
        }
    }

    /// Paint the tab/stack indicator strips (G-6c; D7 glyph-free -- pure
    /// colored segments, never text, never client memory). Tabbed: one
    /// strip row split into per-child segments (1px gap); stacked: one
    /// full-width row per child. The active child's segment lights
    /// FOCUS_COLOR when the focused leaf is inside it, ACTIVE_COLOR
    /// otherwise; the rest are BORDER_COLOR. Repainted with the borders
    /// on focus-only epochs (the highlight follows focus).
    fn paint_strips(&mut self) {
        let dw = self.gpu.width as u64;
        let va = match &self.screen {
            Some(s) => s.va,
            None => return,
        };
        let px = va as *mut u32;
        let fill = |r: Rect, color: u32| {
            if r.is_empty() {
                return;
            }
            // SAFETY: strip areas lie inside their container's rect,
            // which the geometry pass bounds inside the display.
            unsafe {
                for y in r.y..r.y + r.h {
                    for x in r.x..r.x + r.w {
                        *px.add((y as u64 * dw + x as u64) as usize) = color;
                    }
                }
            }
        };
        for (slot, area, mode, children, active) in self.layout.visible_strips() {
            let hot = self.layout.focus_child_of(slot);
            let n = children.len() as u32;
            if n == 0 {
                continue;
            }
            let seg_color = |i: usize| {
                if i == active {
                    if hot == Some(children[i]) {
                        pane::FOCUS_COLOR
                    } else {
                        pane::ACTIVE_COLOR
                    }
                } else {
                    pane::BORDER_COLOR
                }
            };
            match mode {
                Mode::Tabbed => {
                    let each = area.w / n;
                    let mut x = area.x;
                    for (i, _) in children.iter().enumerate() {
                        let w = if i as u32 == n - 1 { area.x + area.w - x } else { each };
                        let gap = if i as u32 == n - 1 || w == 0 { 0 } else { 1 };
                        fill(Rect { x, y: area.y, w: w - gap, h: area.h }, seg_color(i));
                        x += w;
                    }
                }
                Mode::Stacked => {
                    let row_h = pane::TAB_STRIP_H;
                    for (i, _) in children.iter().enumerate() {
                        fill(
                            Rect {
                                x: area.x,
                                y: area.y + (i as u32) * row_h,
                                w: area.w,
                                h: row_h,
                            },
                            seg_color(i),
                        );
                    }
                }
                _ => {}
            }
        }
    }

    /// A signature of the visible leaf geometry (FNV-1a over id + content
    /// rects): structural relayouts change it, focus moves do not.
    fn calc_geom_sig(&self) -> u64 {
        let mut h: u64 = 0xcbf2_9ce4_8422_2325;
        let mut fold = |v: u64| {
            h ^= v;
            h = h.wrapping_mul(0x1_0000_01b3);
        };
        for (slot, id) in self.layout.live_ids() {
            let p = match self.layout.get(slot) {
                Some(p) => p,
                None => continue,
            };
            if !p.visible || !self.layout.is_leaf(slot) {
                continue;
            }
            fold(id as u64);
            let c = p.content;
            fold((c.x as u64) << 32 | c.y as u64);
            fold((c.w as u64) << 32 | c.h as u64);
        }
        h
    }

    /// Push the whole screen buffer to the host resource + display.
    fn screen_flush_full(&mut self) {
        let (dw, dh) = (self.gpu.width, self.gpu.height);
        let res = match &self.screen {
            Some(s) => s.res,
            None => return,
        };
        let _ = self.gpu.transfer(res, 0, 0, 0, dw, dh);
        let _ = self.gpu.flush(res, 0, 0, dw, dh);
    }

    /// Reconcile scanout + chrome with the layout (run after every layout
    /// or hosting mutation). The scanout MODE machine:
    ///   - exactly one visible leaf hosting a display-sized surface ->
    ///     Direct(n), switched at that surface's next present-COMPLETE
    ///     (pending_direct -- the F16 rule, uniformly);
    ///   - anything else visible (splits, letterbox, empty panes) ->
    ///     Composed (the screen resource scans out; presents blit);
    ///   - nothing at all -> Off (Boot stays untouched pre-first-content).
    fn reconcile(&mut self) {
        let (dw, dh) = (self.gpu.width, self.gpu.height);
        self.layout.recompute(dw, dh, self.chords.gaps);
        let vis = self.layout.visible_hosted();
        let nleaves = self.layout.visible_leaf_count();

        let want = if vis.is_empty() && nleaves <= 1 {
            match self.scanout {
                Scanout::Boot => Scanout::Boot,
                _ => Scanout::Off,
            }
        } else if vis.len() == 1 && nleaves == 1 {
            let n = vis[0].1;
            let full = self
                .surf(n)
                .map_or(false, |s| s.w == dw && s.h == dh);
            if full {
                Scanout::Direct(n)
            } else {
                Scanout::Composed
            }
        } else {
            Scanout::Composed
        };

        match want {
            Scanout::Boot => {}
            Scanout::Off => {
                if self.pending_direct.is_some() {
                    say!("tapestryd: scanout off clears pending-direct");
                }
                self.pending_direct = None;
                if self.scanout != Scanout::Off && self.scanout != Scanout::Boot {
                    let _ = self.gpu.set_scanout(0, dw, dh);
                    self.bound_res = 0;
                    self.scanout = Scanout::Off;
                }
            }
            Scanout::Direct(n) => {
                if self.scanout == Scanout::Direct(n) {
                    self.pending_direct = None;
                } else if self.pending_direct != Some(n) {
                    // Defer to n's next present-COMPLETE (F16). Until then
                    // the current scanout (composed frame / boot pattern)
                    // stays -- transitional content, compositor policy.
                    // The edge also emits the redraw CONFIGURE: an
                    // accumulator client's individual slots are patchwork
                    // (only the resource/screen accumulates), so the
                    // switch's full-slot transfer needs a full repaint to
                    // land next. Same-size by construction: Direct(n)
                    // requires the surface display-sized.
                    say!("tapestryd: scanout pending-direct {} ({}x{})", n, dw, dh);
                    self.pending_direct = Some(n);
                    if !self.emit_configure_to(n, dw, dh) {
                        self.retire(n); // wedged; retire clears pending
                    }
                }
            }
            Scanout::Composed => {
                self.pending_direct = None;
                if !self.ensure_screen() {
                    return; // degraded: keep the current scanout; retried
                }
                let entering = self.scanout != Scanout::Composed;
                let sig = self.calc_geom_sig();
                let structural = entering || sig != self.geom_sig;
                if structural {
                    // Structural: full repaint (content blanks; panes heal
                    // by the redraw CONFIGUREs below).
                    self.paint_chrome();
                    self.geom_sig = sig;
                    self.screen_flush_full();
                } else if self.chrome_epoch != self.layout.epoch {
                    // Focus-only: redraw the frames + strip highlights,
                    // keep the content.
                    self.paint_borders();
                    self.paint_strips();
                    self.chrome_epoch = self.layout.epoch;
                    self.screen_flush_full();
                }
                if entering {
                    let sres = self.screen.as_ref().map(|s| s.res).unwrap_or(0);
                    say!("tapestryd: scanout composed ({}x{})", dw, dh);
                    if self.gpu.set_scanout(sres, dw, dh).is_ok() {
                        self.bound_res = sres;
                    }
                    // Flush AFTER the bind (#57): a RESOURCE_FLUSH reaches
                    // only scanouts bound to the resource, so the
                    // screen_flush_full above -- issued while the OLD
                    // scanout was still bound -- was dropped by spec, and
                    // a same-size surface replace renders NOTHING under
                    // the QEMU cocoa frontend (10.0.2 switchSurface swaps
                    // the pixman pointer without a redraw; VNC full-
                    // dirties on replace, which masked this headless).
                    // The post-bind flush makes the switch self-healing
                    // on every frontend.
                    let _ = self.gpu.flush(sres, 0, 0, dw, dh);
                    self.scanout = Scanout::Composed;
                }
                if structural {
                    // The CONFIGURE fan, last (retire recursion-safe at
                    // the tail): every visible hosted surface gets its
                    // pane's CONTENT size -- same-size = the redraw
                    // request, different = the resize offer (G-6b). A
                    // client that ignores an offer keeps its size and is
                    // cropped/letterboxed by the blit clip.
                    let mut wedged: Vec<usize> = Vec::new();
                    for (_, n, c) in self.layout.visible_hosted() {
                        if !self.emit_configure_to(n, c.w, c.h) {
                            wedged.push(n);
                        }
                    }
                    for n in wedged {
                        self.retire(n);
                    }
                }
            }
        }
        self.focus_sync();
    }

    /// Emit the TEV_FOCUS lost/gained pair when the focused surface
    /// changed (G-6c; section 18.4 kind 7, the F5 never-drop class).
    /// value = 1 gained / 0 lost. Runs at every reconcile tail -- every
    /// focus-changing mutation reconciles, so this is the single
    /// emission point; `last_focus` dedups (retire's nested reconcile
    /// re-enters harmlessly).
    fn focus_sync(&mut self) {
        let cur = self.layout.focused_surface();
        if cur == self.last_focus {
            return;
        }
        let prev = self.last_focus;
        self.last_focus = cur; // set first: a wedge-retire below re-enters
        let t = self.tick;
        let focus_ev = |value: u32| Tevent {
            kind: TEV_FOCUS,
            code: 0,
            value,
            rune: 0,
            mods: 0,
            flags: 0,
            tick: t,
        };
        let mut wedged: Vec<usize> = Vec::new();
        if let Some(o) = prev {
            if self.surf(o).is_some() && !self.push_event(o, focus_ev(0)) {
                wedged.push(o);
            }
        }
        if let Some(g) = cur {
            if self.surf(g).is_some() && !self.push_event(g, focus_ev(1)) {
                wedged.push(g);
            }
        }
        for n in wedged {
            self.retire(n);
        }
    }

    /// Blit a presented damage rect from the client's weave slot into the
    /// screen buffer at its pane's content rect (clipped both ways),
    /// returning the SCREEN-space region written (None: hidden /
    /// unhosted / fully clipped). Client weave bytes are read ONLY here,
    /// inside the present dispatch, for the slot the client just
    /// presented -- the G-6 tearing-freedom invariant. The caller pushes
    /// the region device-side (`screen_push`) -- or defers it (HOLD).
    /// Warp-4 (`gl_src`): a GL adoption composes from the BO backing
    /// tapestryd itself mapped -- same geometry as the surface (the
    /// adoption gate), full-image tight stride -- instead of a weave
    /// slot. Every rect/letterbox/crop path below is source-agnostic;
    /// only the base pointer changes.
    fn blit_composed_pixels(
        &mut self,
        n: usize,
        slot: u32,
        x: u32,
        y: u32,
        pw: u32,
        ph: u32,
        gl_src: Option<u64>,
    ) -> Option<Rect> {
        let (sw, slot_stride, weave_va) = match self.surf(n) {
            Some(s) => match &s.weave {
                Some(w) => (s.w, s.slot_stride, w.va),
                None => return None,
            },
            None => return None,
        };
        let content = match self.layout.find_hosting(n) {
            Some(leaf) => match self.layout.get(leaf) {
                Some(p) if p.visible => p.content,
                _ => return None, // hidden: no compose target
            },
            None => return None, // unhosted
        };
        let screen_va = match &self.screen {
            Some(s) => s.va,
            None => return None,
        };
        let dw = self.gpu.width as u64;
        let src_base = match gl_src {
            Some(va) => va,
            None => weave_va + (slot as u64) * slot_stride,
        };
        // Orientation: a GL source needs NO flip. The guest-visible
        // transfer contract is gallium top-down -- osmesa_read_buffer's
        // y_up=FALSE arm copies STRAIGHT and is the shipping llvmpipe
        // path, and the same frontend readback works unchanged on virgl
        // (virglrenderer compensates host-side), so row 0 of the BO
        // backing is the scene TOP exactly like a weave row 0.
        let (sh_full, patchwork) = match self.surf(n) {
            Some(s) => (s.h, s.patchwork),
            None => return None,
        };
        if (sw != content.w || sh_full != content.h) && !patchwork {
            // Fork 2 + the #56 patchwork latch (both user-voted
            // 2026-07-21): a FULL-FRAME presenter (patchwork never
            // latched) LETTERBOXES into its pane -- aspect-preserving
            // scale, up OR down, centered, nearest-neighbor (crisp for
            // the retro-game case; cheap integer math). Damage
            // sub-rects are ignored: any present redraws the FULL
            // scaled rect -- sound exactly BECAUSE the latch is clear:
            // every present so far carried whole-frame bytes. A LATCHED
            // surface is an accumulator (aurora's cell-diff over
            // rotating weave slots): its slots are PATCHWORK, and
            // scaling a full slot composes alternating half-stale
            // frames -- the live-play "utopia pane flipping" bug -- so
            // any size mismatch takes the damage-clipped CROP path
            // below instead. (The pre-#56 discriminator was fit-inside
            // BY SIZE, which cropped a 2px-overflowing split Quake; the
            // present style is the property that actually matters, and
            // it is protocol-observable.) Aurora's pane-tracking resize
            // is the real close (#55). The bars around a letterboxed
            // rect are the pane background, painted by the chrome pass.
            // The geometry comes from the SAME letterbox() ptr_hit
            // inverts -- one authority, no drift.
            if content.w == 0 || content.h == 0 || sh_full == 0 || sw == 0 {
                return None;
            }
            let (ox, oy, dw2, dh2) = Self::letterbox(sw, sh_full, content.w, content.h);
            // One-shot geometry diagnostic (per distinct placement).
            if let Some(su) = self.surf_mut(n) {
                let sig = (ox, oy, dw2, dh2);
                if su.lb_logged != Some(sig) {
                    su.lb_logged = Some(sig);
                    say!(
                        "tapestryd: surface {} letterbox {}x{} -> {}x{} @({},{}) in {}x{}",
                        n, sw, sh_full, dw2, dh2, ox, oy, content.w, content.h
                    );
                }
            }
            // SAFETY: src reads stay inside the weave slot (sx < sw,
            // sy < sh by the division bound: lx < dw2 => lx*sw/dw2 <
            // sw -- ratio math, valid for scale-down as well as up);
            // dst rows stay inside the screen buffer (letterbox()
            // bounds dw2 <= cw and dh2 <= ch by construction in BOTH
            // directions, so the scaled rect is inside content, and
            // content inside the display by the geometry pass).
            unsafe {
                for row in 0..dh2 as u64 {
                    let sy = (row * sh_full as u64) / dh2 as u64;
                    let dy = content.y as u64 + oy as u64 + row;
                    let srow = (src_base + sy * sw as u64 * 4) as *const u32;
                    let drow = (screen_va
                        + (dy * dw + (content.x + ox) as u64) * 4)
                        as *mut u32;
                    for col in 0..dw2 as u64 {
                        let sx = (col * sw as u64) / dw2 as u64;
                        *drow.add(col as usize) = *srow.add(sx as usize);
                    }
                }
            }
            return Some(Rect {
                x: content.x + ox,
                y: content.y + oy,
                w: dw2,
                h: dh2,
            });
        }
        // Same-size fast path: the byte-copy blit, damage-clipped.
        let inter = Rect { x, y, w: pw, h: ph }
            .intersect(Rect { x: 0, y: 0, w: content.w, h: content.h });
        if inter.is_empty() {
            return None;
        }
        // SAFETY: src rows lie within the weave slot (damage was validated
        // against the surface geometry; inter only shrinks it); dst rows
        // lie within the screen buffer (content is inside the display by
        // the geometry pass; inter is inside content).
        unsafe {
            for row in 0..inter.h as u64 {
                let sy = inter.y as u64 + row;
                let dy = content.y as u64 + inter.y as u64 + row;
                let src = (src_base + (sy * sw as u64 + inter.x as u64) * 4) as *const u8;
                let dst = (screen_va + (dy * dw + (content.x + inter.x) as u64) * 4) as *mut u8;
                core::ptr::copy_nonoverlapping(src, dst, inter.w as usize * 4);
            }
        }
        Some(Rect {
            x: content.x + inter.x,
            y: content.y + inter.y,
            w: inter.w,
            h: inter.h,
        })
    }

    /// Push a screen-buffer region to the host resource + display.
    fn screen_push(&mut self, r: Rect) {
        if r.is_empty() {
            return;
        }
        let res = match &self.screen {
            Some(s) => s.res,
            None => return,
        };
        let dw = self.gpu.width as u64;
        let off = ((r.y as u64) * dw + r.x as u64) * 4;
        let _ = self.gpu.transfer(res, off, r.x, r.y, r.w, r.h);
        let _ = self.gpu.flush(res, r.x, r.y, r.w, r.h);
    }

    /// Flush surface `n`'s held region (F13 release; also the implicit
    /// release a non-HOLD present performs). A hold recorded under a
    /// scanout mode that has since changed is DROPPED -- the structural
    /// repaint superseded it (pixels already re-fanned via CONFIGURE).
    fn release_held(&mut self, n: usize) {
        let held = match self.surf_mut(n).and_then(|s| s.held.take()) {
            Some(h) => h,
            None => return,
        };
        match held {
            Held::Direct(r) => {
                if self.scanout == Scanout::Direct(n) {
                    if let Some(res) = self.surf(n).map(|s| s.resource_id) {
                        let _ = self.gpu.flush(res, r.x, r.y, r.w, r.h);
                    }
                }
            }
            Held::Composed(r) => {
                if self.scanout == Scanout::Composed {
                    self.screen_push(r);
                }
            }
        }
    }

    /// The `layout` file grammar (G-6): `<verb> <pane-id> [args]` --
    /// `split <id> h|v`, `close <id>`, `focus <id>`, `mode <id> <mode>`,
    /// `move <id> <dir>`, `zoom <id>` -- plus the id-less verbs acting on
    /// the focused leaf (G-6c): `focusdir <dir>`, `tab next|prev`.
    pub fn layout_cmd(&mut self, s: &str) -> Result<(), u32> {
        let s = s.trim();
        let mut it = s.splitn(2, ' ');
        let verb = it.next().ok_or(p9::E_INVAL)?;
        let rest = it.next().unwrap_or("").trim();
        match verb {
            "focusdir" => {
                let d = Dir::parse(rest).ok_or(p9::E_INVAL)?;
                // A miss (screen edge; zoomed) is a no-op, not an error --
                // the chord ergonomic.
                if self.layout.focus_dir(d) {
                    self.reconcile();
                }
                return Ok(());
            }
            "tab" => {
                let fwd = match rest {
                    "next" => true,
                    "prev" => false,
                    _ => return Err(p9::E_INVAL),
                };
                // Revealing another tab is meaningless zoomed: restore
                // the layout first (the tmux rule).
                self.layout.unzoom();
                if self.layout.tab_cycle(fwd) {
                    self.reconcile();
                }
                return Ok(());
            }
            _ => {}
        }
        let mut it2 = rest.splitn(2, ' ');
        let id: u32 = it2
            .next()
            .and_then(|t| t.trim().parse().ok())
            .ok_or(p9::E_INVAL)?;
        let args = it2.next().unwrap_or("").trim();
        let cmd = match verb {
            "split" | "mode" | "move" => {
                if args.is_empty() {
                    return Err(p9::E_INVAL);
                }
                alloc::format!("{} {}", verb, args)
            }
            "close" | "focus" | "zoom" => {
                if !args.is_empty() {
                    return Err(p9::E_INVAL);
                }
                String::from(verb)
            }
            _ => return Err(p9::E_INVAL),
        };
        self.pane_cmd(id, &cmd)
    }

    /// One layout mutation targeting pane `id` (shared by the layout file
    /// and each pane's ctl). Every successful mutation reconciles.
    /// Structural verbs restore a zoomed layout first (the tmux rule);
    /// `focus` keeps zoom only when it names the zoomed pane itself.
    pub fn pane_cmd(&mut self, id: u32, cmd: &str) -> Result<(), u32> {
        let slot = self.layout.slot_of_id(id).ok_or(p9::E_NOENT)?;
        let cmd = cmd.trim();
        if let Some(rest) = cmd.strip_prefix("split ") {
            let mode = match rest.trim() {
                "h" => Mode::SplitH,
                "v" => Mode::SplitV,
                _ => return Err(p9::E_INVAL),
            };
            if !self.layout.is_leaf(slot) {
                return Err(p9::E_INVAL);
            }
            self.layout.unzoom();
            self.layout.split(slot, mode).ok_or(p9::E_NOMEM)?;
        } else if let Some(rest) = cmd.strip_prefix("move ") {
            let d = Dir::parse(rest.trim()).ok_or(p9::E_INVAL)?;
            self.layout.unzoom();
            if !self.layout.move_dir(slot, d) {
                return Err(p9::E_INVAL);
            }
        } else if cmd == "zoom" {
            if !self.layout.zoom_toggle(slot) {
                return Err(p9::E_INVAL);
            }
        } else if cmd == "close" {
            // Closing a pane strands its surfaces invisible BY DESIGN
            // (hosting is once-per-life, at create) and asks each
            // stranded client to exit via the queued TEV_CLOSE (G-6b).
            // The surface stays live until the client destroys it or its
            // conn tears down -- a compositor-initiated pane close is a
            // request, never a forced retire (the client may need to
            // save). The event is non-droppable; a wedge force-retires.
            self.layout.unzoom();
            let unhosted = self.layout.close(slot);
            for n in unhosted {
                self.send_close(n);
            }
        } else if cmd == "focus" {
            if self.layout.zoom_id() != Some(id) {
                self.layout.unzoom();
            }
            if !self.layout.focus(slot) {
                return Err(p9::E_INVAL);
            }
        } else if let Some(m) = cmd.strip_prefix("mode ") {
            let mode = Mode::parse(m.trim()).ok_or(p9::E_INVAL)?;
            self.layout.unzoom();
            if !self.layout.set_mode(slot, mode) {
                return Err(p9::E_INVAL);
            }
        } else {
            return Err(p9::E_INVAL);
        }
        self.reconcile();
        Ok(())
    }

    /// The retire (spec Destroy -> ServerRelease -> Free, server side).
    /// See the file header for the I-40 ordering this realizes.
    fn retire(&mut self, n: usize) {
        let s = match self.surfaces.get_mut(n).and_then(|s| s.take()) {
            Some(s) => s,
            None => return,
        };
        say!("tapestryd: retire surface {}", n);
        // A stale last_focus naming this slot would suppress the gained
        // event for a FUTURE surface minted into it -- clear it (the
        // reconcile below re-emits for whatever takes focus).
        if self.last_focus == Some(n) {
            self.last_focus = None;
        }
        // (0) The pane side (G-6): the hosting leaf closes (single-child
        // containers collapse; the root collapses to an empty leaf). Done
        // BEFORE reconcile so the layout no longer names n.
        if let Some(leaf) = self.layout.find_hosting(n) {
            let _ = self.layout.close(leaf);
        }
        // (1) Quiesce: presents are handled synchronously (see header) --
        // the in-flight set is empty here by construction.
        if let Some(w) = &s.weave {
            // (2) Disarm BEFORE any backing free: registry-removal-before-
            // page-free (R2-F5). A consumed (claimed) share is a harmless
            // miss; an unclaimed one is removed so no Tweft claim can race
            // the retire onto a dying weave (the spec's NoStaleMap).
            if let Some(id) = w.share_id {
                let rc = unsafe { t_weft_unshare(id) };
                if rc < 0 {
                    // Already claimed (consumed at Map) -- expected.
                }
            }
            // (3) Scanout release BEFORE the resource dies: reconcile moves
            // scanout off n (the layout no longer names it). Two arms can
            // leave scanout still referencing n's resource -- a want of
            // Direct(survivor) (deferred to the survivor's present, F16)
            // and a degraded Composed entry (screen alloc failed) -- so
            // force it away explicitly in both.
            if self.pending_direct == Some(n) {
                self.pending_direct = None;
            }
            self.reconcile();
            if self.scanout == Scanout::Direct(n) {
                let (dw, dh) = (self.gpu.width, self.gpu.height);
                let _ = self.gpu.set_scanout(0, dw, dh);
                self.bound_res = 0;
                self.scanout = Scanout::Off;
            }
            // Warp-4: a GL-adopted scanout can survive the arms above --
            // the soft-Off retarget window leaves the DEVICE bound to the
            // adopted BO while `scanout` already reads Off. If the device
            // still shows a BO consented to THIS surface incarnation,
            // unbind: the display must not outlive the surface it was
            // granted to. (The BO itself is the ctx's to free; only the
            // binding is ours.)
            let gl_bound = self.bound_res != 0
                && self.warp_ctxs.iter().flatten().any(|c| match c.present_to {
                    Some((sl, g, bp)) if sl == n && g == s.gen => c
                        .bos
                        .iter()
                        .flatten()
                        .any(|b| b.pub_id == bp && b.res_id == self.bound_res),
                    _ => false,
                });
            if gl_bound {
                let (dw, dh) = (self.gpu.width, self.gpu.height);
                let _ = self.gpu.set_scanout(0, dw, dh);
                self.bound_res = 0;
            }
            // (4) The GPU resource dies before its backing.
            let _ = self.gpu.detach_backing(s.resource_id);
            let _ = self.gpu.resource_unref(s.resource_id);
            // (5) Drop the server refs: unmap our own mapping, close the
            // weave handle (serverRef -> FALSE; #847 keeps the pages until
            // the client's mapping ref drops too).
            unsafe { t_burrow_detach(w.va, w.size) };
            unsafe { t_close(w.handle) };
        }
        // A displaced generation still draining (resize acked, no present
        // yet) dies with the surface -- same per-generation order; its
        // resource was never scanned out (only a post-fence present could
        // have made it visible, and that present would have retired it).
        if let Some((oldw, old_res)) = s.old_weave {
            self.release_gen(&oldw, old_res);
        }
        // No diagnostic (#55b): a surface retire is routine steady-state
        // traffic (every client exit / pane close), and with a live-acking
        // fbcon it lands concurrent with session output -- a SYS_PUTS line
        // here interleaves at the UART FIFO (the P1-F carve-out) and tears
        // byte patterns mid-line (it split `/home/michael` in the panes
        // post-battery assert). The error/edge prints above stay.
        let _ = s.presents;
    }

    /// Retire every surface owned by a dying conn (teardown / Tversion).
    fn retire_conn(&mut self, conn_id: u64) {
        for n in 0..MAX_SURFACES {
            if self.surf(n).map_or(false, |s| s.owner_conn == conn_id) {
                self.retire(n);
            }
        }
    }

    /// Queue an event on surface `n` under the R2-F4 policy. Returns false
    /// if the push WEDGED the surface (caller must retire it).
    fn push_event(&mut self, n: usize, ev: Tevent) -> bool {
        let s = match self.surf_mut(n) {
            Some(s) => s,
            None => return true,
        };
        if ev.kind == TEV_FRAME {
            // Coalesce GLOBALLY: at most one FRAME queued per surface (the
            // G-3-audit F3 fix -- a back-of-queue-only check let interleaved
            // KEY/FRAME streams accumulate FRAMEs). Refresh the queued one's
            // tick in place; the scan is bounded by EVENT_QUEUE_CAP.
            if let Some(f) = s.events.iter_mut().find(|e| e.kind == TEV_FRAME) {
                f.tick = ev.tick;
                return true;
            }
            if s.events.len() >= EVENT_QUEUE_CAP {
                return true; // droppable class: drop the new FRAME
            }
            s.events.push_back(ev);
            return true;
        }
        if ev.kind == TEV_PTR_REL {
            // Deltas are ADDITIVE: replacing (the MOVE discipline) loses
            // motion, so a back-of-queue REL sums instead (i16-saturating;
            // back-of-queue only -- an interleaved event starts a fresh
            // record, preserving order). Overflow falls through to the
            // droppable class below.
            if let Some(t) = s.events.back_mut().filter(|e| e.kind == TEV_PTR_REL) {
                let sx = (t.value >> 16) as u16 as i16 as i32
                    + (ev.value >> 16) as u16 as i16 as i32;
                let sy = (t.value & 0xFFFF) as u16 as i16 as i32
                    + (ev.value & 0xFFFF) as u16 as i16 as i32;
                let sx = sx.clamp(-32768, 32767) as i16 as u16 as u32;
                let sy = sy.clamp(-32768, 32767) as i16 as u16 as u32;
                t.value = (sx << 16) | sy;
                t.mods = ev.mods;
                t.tick = ev.tick;
                return true;
            }
        }
        if ev.kind == TEV_CONFIGURE {
            // Unacked CONFIGUREs coalesce -- only the latest serial matters
            // (section 18.3): replace a queued unread one WHOLESALE.
            if let Some(c) = s.events.iter_mut().find(|e| e.kind == TEV_CONFIGURE) {
                *c = ev;
                return true;
            }
            // Falls through to the non-droppable push below.
        }
        if s.events.len() >= EVENT_QUEUE_CAP {
            // Evict one coalescible to make room for the non-droppable.
            if let Some(i) = s.events.iter().position(|e| e.coalescible()) {
                s.events.remove(i);
            } else {
                // R2-F4: non-droppables alone fill the bounded buffer --
                // the client is dead/stalled; WEDGE (force-retire).
                say!("tapestryd: surface {} WEDGED (event overflow)", n);
                return false;
            }
        }
        s.events.push_back(ev);
        true
    }

    /// Roll the #164 present-pressure buckets forward to now. Lazy: both
    /// readers/writers (`note_present`, `animating`) call it first. The
    /// staleness guard is the `>= 2 windows` arm itself -- it clears
    /// both buckets at the FIRST later call, however long the gap
    /// (audit F3: `animating` runs only in the frozen==false,
    /// input-quiet regime, so "called every pass" holds only where its
    /// value is read; do not remove the clear as redundant). The
    /// promote arm carries exactly one window forward.
    fn roll_present_buckets(&mut self) {
        match self.present_bucket_start {
            None => self.present_bucket_start = Some(Instant::now()),
            Some(t0) => {
                let age = t0.elapsed().as_millis() as u64;
                if age >= 2 * PRESENT_BURST_WINDOW_MS {
                    self.present_prev_count = 0;
                    self.present_bucket_count = 0;
                    self.present_bucket_start = Some(Instant::now());
                } else if age >= PRESENT_BURST_WINDOW_MS {
                    self.present_prev_count = self.present_bucket_count;
                    self.present_bucket_count = 0;
                    self.present_bucket_start = Some(Instant::now());
                }
            }
        }
    }

    /// A well-formed present landed on surface `n`. It counts toward the
    /// #164 pressure ONLY if it can change the screen (audit F1): a
    /// HIDDEN pane's paced client still presents ~20/s via the SDL
    /// pacer's 50 ms timeout ("timeout pacing"), does zero visible work
    /// (blit_composed_pixels returns None for it), and must not hold the
    /// clock -- else a game tabbed to the background pins 60 Hz on an
    /// idle console and "a paced client naturally suspends while hidden"
    /// inverts. Screen-changing = completing a direct-scanout switch,
    /// being the scanned-out surface, or layout-visible (the same
    /// predicate the compositor's own blit uses).
    pub fn note_present(&mut self, n: usize) {
        let visible = self.pending_direct == Some(n)
            || self.scanout == Scanout::Direct(n)
            || self
                .layout
                .find_hosting(n)
                .and_then(|leaf| self.layout.get(leaf))
                .map_or(false, |p| p.visible);
        if !visible {
            return;
        }
        self.roll_present_buckets();
        self.present_bucket_count = self.present_bucket_count.saturating_add(1);
    }

    /// Is enough SCREEN-CHANGING present pressure arriving to count as
    /// animation? (The #164 activity axis: ORed with input recency by
    /// the main loop's tick-rate decision.) The count is a GLOBAL sum
    /// across all visible surfaces (audit F2), not per-client: N
    /// sub-threshold visible presenters compose, so e.g. the blink plus
    /// a visible ~5 fps client in a split hold the clock together --
    /// acceptable, since jointly they ARE visible animation; the blink
    /// margin claim assumes aurora is the only idle-state presenter,
    /// which holds at a settled prompt. A present-spamming visible
    /// client pins the clock at the ctl rate -- exactly the
    /// pre-throttle baseline, bounded, and fast visible presents ARE
    /// activity by definition.
    pub fn animating(&mut self) -> bool {
        self.roll_present_buckets();
        self.present_bucket_count + self.present_prev_count >= PRESENT_BURST_MIN
    }

    /// Emit the FRAME tick to every VISIBLE hosted surface (G-6: hidden
    /// surfaces -- tab-background, unhosted -- get no pacing signal; a
    /// paced client naturally suspends while hidden). Wedged surfaces
    /// retire inline.
    pub fn frame_tick(&mut self) {
        self.tick += 1;
        let t = self.tick;
        let vis: Vec<usize> = self.layout.visible_hosted().iter().map(|v| v.1).collect();
        for n in vis {
            let ev = Tevent {
                kind: TEV_FRAME,
                code: 0,
                value: 0,
                rune: 0,
                mods: 0,
                flags: 0,
                tick: t,
            };
            if !self.push_event(n, ev) {
                self.retire(n);
            }
        }
    }

    /// Emit CONFIGURE {serial, W<<16|H} to surface `n` (sections 18.3 +
    /// 18.4), recording it as the surface's ackable offer. A SAME-size
    /// CONFIGURE is the REDRAW request (a structural repaint blanks pane
    /// content; an accumulator client heals only by a full repaint); a
    /// DIFFERENT-size one is the resize offer the `resize W H <serial>`
    /// ack answers (G-6b). Coalesce-by-replacement in the queue + the
    /// single `offered` slot both encode "only the latest matters".
    /// Returns push_event's wedge verdict (false = caller must retire).
    fn emit_configure_to(&mut self, n: usize, w: u32, h: u32) -> bool {
        if w == 0 || h == 0 || w > 0xffff || h > 0xffff {
            return true; // degenerate pane: nothing showable to offer
        }
        let t = self.tick;
        let serial = match self.surf_mut(n) {
            Some(s) => {
                s.cfg_serial = s.cfg_serial.wrapping_add(1);
                s.offered = Some((s.cfg_serial, w, h));
                s.cfg_serial
            }
            None => return true,
        };
        let ev = Tevent {
            kind: TEV_CONFIGURE,
            code: serial,
            value: (w << 16) | h,
            rune: 0,
            mods: 0,
            flags: 0,
            tick: t,
        };
        self.push_event(n, ev)
    }

    /// Queue TEV_CLOSE on surface `n` (its pane closed under it -- the
    /// exit request). Wedged surfaces retire inline (R2-F4).
    fn send_close(&mut self, n: usize) {
        let ev = Tevent {
            kind: TEV_CLOSE,
            code: 0,
            value: 0,
            rune: 0,
            mods: 0,
            flags: 0,
            tick: self.tick,
        };
        if !self.push_event(n, ev) {
            self.retire(n);
        }
    }

    /// Deliver a key to the FOCUSED leaf's surface (G-6 routing).
    pub fn key_event(&mut self, code: u16, value: u32, rune: u32, mods: u16) {
        let n = match self.layout.focused_surface() {
            Some(n) => n,
            None => return, // no focused surface; input drops
        };
        let ev = Tevent {
            kind: TEV_KEY,
            code,
            value,
            rune,
            mods,
            flags: 0,
            tick: self.tick,
        };
        if !self.push_event(n, ev) {
            self.retire(n);
        }
    }

    /// The letterbox placement of a (sw, sh) surface inside a (cw, ch)
    /// pane content rect: aspect-preserving scale + center (the fork-2
    /// decision, user-voted 2026-07-21). Returns (ox, oy, dw2, dh2) --
    /// the scaled rect's content-relative origin + dims. Equal dims
    /// return the identity (0, 0, cw, ch). THE ONE GEOMETRY AUTHORITY:
    /// blit_composed_pixels' forward map and ptr_hit's inverse both
    /// derive from this, so they cannot drift apart (the G-7c audit-F3
    /// lesson made structural).
    fn letterbox(sw: u32, sh: u32, cw: u32, ch: u32) -> (u32, u32, u32, u32) {
        if sw == cw && sh == ch {
            return (0, 0, cw, ch);
        }
        // Width-bound iff cw/sw <= ch/sh  <=>  cw*sh <= ch*sw (u64: no
        // overflow for display-scale dims).
        let (dw2, dh2) = if (cw as u64) * (sh as u64) <= (ch as u64) * (sw as u64) {
            (cw, (((sh as u64) * (cw as u64)) / (sw as u64).max(1)) as u32)
        } else {
            ((((sw as u64) * (ch as u64)) / (sh as u64).max(1)) as u32, ch)
        };
        let (dw2, dh2) = (dw2.max(1), dh2.max(1));
        ((cw - dw2) / 2, (ch - dh2) / 2, dw2, dh2)
    }

    /// The surface under display point (px, py) + the point translated to
    /// surface-relative coords (G-7c pointer routing: under-the-pointer,
    /// NOT the focused leaf -- clicking a pane must land in that pane;
    /// keyboard focus stays chord-driven, no click-to-focus at this
    /// stage). A full-frame presenter letterboxes into its pane (fork 2
    /// + the #56 patchwork latch), so the inverse subtracts the content
    /// + letterbox origins and UNSCALES -- via the same letterbox() the
    /// blit uses. A point over the bars CLAMPS into the scaled rect,
    /// keeping drag/mouse-look deltas alive at the boundary.
    fn ptr_hit(&self, px: u32, py: u32) -> Option<(usize, u16, u16)> {
        let (n, c) = self.layout.surface_at(px, py)?;
        let s = self.surf(n)?;
        if s.w == 0 || s.h == 0 || c.w == 0 || c.h == 0 {
            return None;
        }
        if s.patchwork {
            // Latched accumulator = the CROP placement (see
            // blit_composed_pixels): surface (0,0) at the content
            // origin, damage-clipped. The inverse is the plain subtract
            // + far-edge clamp (which also covers a patchwork surface
            // SMALLER than its pane: a point past the surface extent
            // clamps to the far edge, keeping deltas alive).
            let sx = (px - c.x).min(s.w - 1).min(0xFFFF) as u16;
            let sy = (py - c.y).min(s.h - 1).min(0xFFFF) as u16;
            return Some((n, sx, sy));
        }
        let (ox, oy, dw2, dh2) = Self::letterbox(s.w, s.h, c.w, c.h);
        // Content-relative, clamped into the letterbox rect, unscaled.
        let lx = (px - c.x).saturating_sub(ox).min(dw2 - 1);
        let ly = (py - c.y).saturating_sub(oy).min(dh2 - 1);
        let sx = (((lx as u64) * (s.w as u64)) / (dw2 as u64)) as u32;
        let sy = (((ly as u64) * (s.h as u64)) / (dh2 as u64)) as u32;
        let sx = sx.min(s.w - 1).min(0xFFFF) as u16;
        let sy = sy.min(s.h - 1).min(0xFFFF) as u16;
        Some((n, sx, sy))
    }

    /// ABSOLUTE pointer motion at display coords (G-7c; the tablet
    /// drain). Also synthesizes the TEV_PTR_REL delta from the previous
    /// abs position -- the abs-only-frontend mouse-look path (QEMU cocoa
    /// with a tablet present never produces host rel events); the first
    /// abs motion only seeds the base. Edge-stall is inherent to an abs
    /// source (the host cursor stops at the window edge); the relative
    /// device is exact.
    pub fn ptr_move(&mut self, px: u32, py: u32, mods: u16) {
        if let Some((lx, ly)) = self.abs_last {
            let (dx, dy) = (px as i32 - lx as i32, py as i32 - ly as i32);
            if dx != 0 || dy != 0 {
                self.ptr_rel_emit(dx, dy, mods);
            }
        }
        self.abs_last = Some((px, py));
        self.ptr_commit(px, py, mods);
    }

    /// RELATIVE pointer motion (the mouse drain): emit the EXACT deltas
    /// to the focused surface (unclamped -- mouse-look must not stall at
    /// the display edge), then accumulate into the pointer position so
    /// button/click routing follows the relative device too. abs_last is
    /// untouched (per-source delta frames).
    pub fn ptr_move_rel(&mut self, dx: i32, dy: i32, mods: u16) {
        self.ptr_rel_emit(dx, dy, mods);
        let (dw, dh) = (self.gpu.width as i32, self.gpu.height as i32);
        // Saturating (round-4 F2): `dx`/`dy` arrive from the device via a
        // saturating accumulator, so their ceiling IS i32::MAX -- a plain
        // `+` here overflows for any ptr_x >= 1 and, under
        // overflow-checks + panic=abort, kills the console. Round 3's
        // commit message claimed this line was fixed; it was not.
        let px = (self.ptr_x as i32).saturating_add(dx).clamp(0, dw.max(1) - 1) as u32;
        let py = (self.ptr_y as i32).saturating_add(dy).clamp(0, dh.max(1) - 1) as u32;
        self.ptr_commit(px, py, mods);
    }

    /// Deliver a TEV_PTR_REL to the FOCUSED leaf's surface (mouse-look is
    /// a focus companion like keys, decoupled from the pointer position;
    /// PTR_MOVE keeps the under-pointer rule). Deltas clamp to i16.
    fn ptr_rel_emit(&mut self, dx: i32, dy: i32, mods: u16) {
        let n = match self.layout.focused_surface() {
            Some(n) => n,
            None => return,
        };
        let vx = dx.clamp(-32768, 32767) as i16 as u16 as u32;
        let vy = dy.clamp(-32768, 32767) as i16 as u16 as u32;
        let ev = Tevent {
            kind: TEV_PTR_REL,
            code: 0,
            value: (vx << 16) | vy,
            rune: 0,
            mods,
            flags: 0,
            tick: self.tick,
        };
        if !self.push_event(n, ev) {
            self.retire(n);
        }
    }

    /// The shared position commit: MOVE is the coalescible class (R2-F4):
    /// an overflowing queue evicts it, never a control event, so a motion
    /// burst cannot WEDGE a surface.
    fn ptr_commit(&mut self, px: u32, py: u32, mods: u16) {
        self.ptr_x = px;
        self.ptr_y = py;
        if let Some((n, sx, sy)) = self.ptr_hit(px, py) {
            let ev = Tevent {
                kind: TEV_PTR_MOVE,
                code: 0,
                value: ((sx as u32) << 16) | sy as u32,
                rune: 0,
                mods,
                flags: 0,
                tick: self.tick,
            };
            if !self.push_event(n, ev) {
                self.retire(n);
            }
        }
    }

    /// Pointer button (evdev BTN_*) at the current pointer position.
    /// Non-droppable (a lost release strands a drag).
    pub fn ptr_btn(&mut self, code: u16, pressed: bool, mods: u16) {
        if let Some((n, _, _)) = self.ptr_hit(self.ptr_x, self.ptr_y) {
            let ev = Tevent {
                kind: TEV_PTR_BTN,
                code,
                value: pressed as u32,
                rune: 0,
                mods,
                flags: 0,
                tick: self.tick,
            };
            if !self.push_event(n, ev) {
                self.retire(n);
            }
        }
    }

    /// Wheel scroll (signed delta) at the current pointer position.
    /// Non-droppable (discrete steps; losing one skips content).
    pub fn ptr_scroll(&mut self, delta: i32, mods: u16) {
        if let Some((n, _, _)) = self.ptr_hit(self.ptr_x, self.ptr_y) {
            let ev = Tevent {
                kind: TEV_SCROLL,
                code: 0,
                value: delta as u32,
                rune: 0,
                mods,
                flags: 0,
                tick: self.tick,
            };
            if !self.push_event(n, ev) {
                self.retire(n);
            }
        }
    }

    fn chord_bit(&self, code: u16) -> bool {
        let i = (code as usize) & 0xff;
        self.chord_down[i / 64] & (1 << (i % 64)) != 0
    }
    fn chord_bit_set(&mut self, code: u16, on: bool) {
        let i = (code as usize) & 0xff;
        if on {
            self.chord_down[i / 64] |= 1 << (i % 64);
        } else {
            self.chord_down[i / 64] &= !(1 << (i % 64));
        }
    }

    /// The Super chord layer (G-6c; sections 14 + 18.4): the compositor's
    /// reserved-modifier plane, intercepted ABOVE the event stream. While
    /// Super is held, EVERY non-modifier key is compositor input -- bound
    /// chords act, unbound ones drop; none reaches a surface (the whole
    /// plane is reserved, so no client can ever come to depend on a Super
    /// combo). A swallowed press swallows its release/repeats too, even
    /// if Super lifted first (no stray release reaches a client); a key
    /// pressed BEFORE Super went down keeps flowing (its release must
    /// reach the client that saw its press). Returns true = consumed.
    /// The caller filters modifier keys (they flow -- clients see mods).
    pub fn chord_key(&mut self, code: u16, value: u32, mods: u16) -> bool {
        if value == 0 {
            // Release: consume iff its press was swallowed.
            if self.chord_bit(code) {
                self.chord_bit_set(code, false);
                return true;
            }
            return false;
        }
        let super_held = mods & crate::keymap::MOD_SUPER != 0;
        if value == 2 {
            // Repeat: follows its press's disposition; a repeat while
            // Super is held is plane-reserved regardless.
            return self.chord_bit(code) || super_held;
        }
        if !super_held {
            return false;
        }
        self.chord_bit_set(code, true);
        self.chord_action(code, mods & crate::keymap::MOD_SHIFT != 0);
        true
    }

    /// Dispatch one Super chord: look the (code, shift) up in the RUNTIME
    /// table (cfg-4) and execute the bound action, if any. An unbound key
    /// is plane-reserved + dropped (unchanged). The table is seeded with
    /// the stage-0 i3-flavored defaults and remapped by the gated `chord`
    /// ctl verb; the lookup here is the ONLY consumer, so a rebind takes
    /// effect on the next press with no other coupling.
    fn chord_action(&mut self, code: u16, shift: bool) {
        if let Some(action) = self.chords.lookup(code, shift) {
            self.exec_chord(action);
        }
    }

    /// Perform one resolved chord action against the layout (the old
    /// hardcoded arms, now keyed by ChordAction). A structural change
    /// reconciles; a no-op (edge/degenerate) does not.
    fn exec_chord(&mut self, action: ChordAction) {
        match action {
            ChordAction::FocusDir(d) => {
                if self.layout.focus_dir(d) {
                    self.reconcile();
                }
            }
            ChordAction::MoveDir(d) => {
                let f = self.layout.focused;
                self.layout.unzoom();
                if self.layout.move_dir(f, d) {
                    self.reconcile();
                }
            }
            ChordAction::Split(mode) => {
                self.layout.unzoom();
                let f = self.layout.focused;
                if self.layout.split(f, mode).is_some() {
                    self.reconcile();
                }
            }
            ChordAction::Zoom => {
                let f = self.layout.focused;
                if self.layout.zoom_toggle(f) {
                    self.reconcile();
                }
            }
            ChordAction::SetMode(mode) => {
                self.layout.unzoom();
                let f = self.layout.focused;
                if self.layout.set_mode(f, mode) {
                    self.reconcile();
                }
            }
            ChordAction::SplitToggle => {
                // Split-orientation toggle on the focused leaf's parent.
                let f = self.layout.focused;
                let parent_mode = self
                    .layout
                    .get(f)
                    .and_then(|p| p.parent)
                    .and_then(|pi| match self.layout.get(pi).map(|p| &p.kind) {
                        Some(pane::Kind::Container { mode, .. }) => Some(*mode),
                        _ => None,
                    });
                let want = match parent_mode {
                    Some(Mode::SplitH) => Mode::SplitV,
                    _ => Mode::SplitH,
                };
                self.layout.unzoom();
                if self.layout.set_mode(f, want) {
                    self.reconcile();
                }
            }
            ChordAction::TabCycle(fwd) => {
                self.layout.unzoom();
                if self.layout.tab_cycle(fwd) {
                    self.reconcile();
                }
            }
            ChordAction::Close => {
                let f = self.layout.focused;
                if let Some(id) = self.layout.id_of(f) {
                    let _ = self.pane_cmd(id, "close");
                }
            }
        }
    }

    fn live_count(&self) -> usize {
        self.surfaces.iter().filter(|s| s.is_some()).count()
    }
}

// =============================================================================
// The connection (the ptyfs Conn shape + the netd deferral).
// =============================================================================

#[derive(Clone, Copy)]
struct Fid {
    fid: u32,
    path: u64,
    gen: u32, // the surface generation captured at bind (0 for static qids)
    opened: bool,
}

enum Disp {
    Reply(usize),
    Deferred,
    Fatal,
}

#[derive(Clone, Copy)]
struct PendingRead {
    fid: u32,
    surf: usize,
    gen: u32,
    tag: u16,
    cap: usize,
}

/// A parked fence-file read (W2d): delivered by poll_fences when its ctx
/// posts a newer signaled id (or EOFs when the ctx dies). No cap field --
/// the park guard already required room for a whole record.
#[derive(Clone, Copy)]
struct PendingFence {
    fid: u32,
    ctx_pub: u32,
    tag: u16,
}

/// The largest fence record ("<u64 max>\n" = 21 bytes): the park guard's
/// floor, like FK_EVENT's TEVENT_LEN.
const FENCE_REC_MAX: usize = 21;

const WARP_NO_CTX: Option<WarpCtx> = None;

/// Heap BO row (#204: 1024-wide rows outgrew the daemon's stack).
/// `try_reserve` so an OOM fails the caller (the ctx MINT) clean instead
/// of aborting the compositor; `resize_with` within reserved capacity
/// never reallocates.
fn warp_bo_row() -> Option<alloc::boxed::Box<[Option<WarpBo>]>> {
    let mut v: Vec<Option<WarpBo>> = Vec::new();
    if v.try_reserve_exact(MAX_WARP_BOS_PER_CTX).is_err() {
        return None;
    }
    v.resize_with(MAX_WARP_BOS_PER_CTX, || None);
    Some(v.into_boxed_slice())
}

// --- Warp-2c: the GPU-seam object lifecycle -------------------------------
impl Comp {
    fn wctx_slot(&self, pub_id: u32) -> Option<usize> {
        self.warp_ctxs
            .iter()
            .position(|c| c.as_ref().map_or(false, |c| c.pub_id == pub_id))
    }

    /// Resolve a live ctx the CALLER owns (the F2 gate, warp edition).
    fn wctx(&self, pub_id: u32, conn: u64) -> Option<&WarpCtx> {
        let c = self.warp_ctxs[self.wctx_slot(pub_id)?].as_ref().unwrap();
        if c.owner_conn != conn || c.retiring {
            return None;
        }
        Some(c)
    }

    fn wctx_mut(&mut self, pub_id: u32, conn: u64) -> Option<&mut WarpCtx> {
        let i = self.wctx_slot(pub_id)?;
        let c = self.warp_ctxs[i].as_mut().unwrap();
        if c.owner_conn != conn || c.retiring {
            return None;
        }
        Some(c)
    }

    /// Resolve a live BO (and its ctx pub id) the caller owns.
    fn wbo(&self, bo_pub: u32, conn: u64) -> Option<(&WarpCtx, &WarpBo)> {
        for c in self.warp_ctxs.iter().flatten() {
            if c.owner_conn != conn || c.retiring {
                continue;
            }
            for b in c.bos.iter().flatten() {
                if b.pub_id == bo_pub && !b.retiring {
                    return Some((c, b));
                }
            }
        }
        None
    }

    /// This conn's ctx pub_id, if it has one (#178: the harness levers are
    /// scoped through it). `wctx_mint` enforces one ctx per client, so the
    /// first match IS the answer.
    #[cfg(feature = "test-mode")]
    fn wctx_of_conn(&self, conn: u64) -> Option<u32> {
        self.warp_ctxs
            .iter()
            .flatten()
            .find(|c| c.owner_conn == conn)
            .map(|c| c.pub_id)
    }

    /// Warp-4: the ACTIVE GL adoption for surface `n`, or None. Active =
    /// the surface names a live ctx (`glsrc`), that ctx's consent names
    /// this surface INCARNATION back (slot + gen), the consented BO is
    /// alive on the ctx, and its geometry equals the surface's. Ownership
    /// needs no check here: each half was written through its owner-gated
    /// ctl, and pub-id resolution + the gen pin make a stale half inert.
    /// Resolved fresh at every use -- either side's death simply makes
    /// this return None (the routing then falls back to the 2D arms).
    fn gl_adoption(&self, n: usize) -> Option<GlAdopt> {
        let s = self.surfaces.get(n)?.as_ref()?;
        let want_ctx = s.gl_src?;
        let c = self
            .warp_ctxs
            .iter()
            .flatten()
            .find(|c| c.pub_id == want_ctx && !c.retiring)?;
        // ROUND-3 F2: the SAME synchronous-lane hazard the probe's `verify`
        // is gated on -- and this path runs it EVERY FRAME, unbidden by the
        // client that owns the ctx, where the probe runs it on demand. Both
        // callers below drive `.step` on THIS client's dev_ctx (the composed
        // arm's `transfer_from_3d_sync`, the direct arm's `flush`), so a ctx
        // whose fence was abandoned 30 s ago -- by definition GL work that
        // has not finished -- would take the readback into the 500 ms
        // deadline and latch the engine `dead`, terminal, for every client
        // and the console. The gate that existed on the verb did not make
        // the lane safe; it made one caller safe.
        //
        // `fence_poisoned` ONLY, deliberately not `fences_in_flight`: a
        // healthily-rendering client has fences outstanding most of the
        // time, so gating on those would collapse composition to the 2D
        // weave constantly. Returning `None` degrades to a stale frame,
        // which both call sites already handle.
        if c.fence_poisoned {
            return None;
        }
        let (slot, gen, bo_pub) = c.present_to?;
        if slot != n || gen != s.gen {
            return None;
        }
        let b = c
            .bos
            .iter()
            .flatten()
            .find(|b| b.pub_id == bo_pub && !b.retiring && b.dma_fd >= 0)?;
        if b.w != s.w || b.h != s.h {
            return None;
        }
        Some(GlAdopt {
            dev_ctx: c.dev_ctx,
            res_id: b.res_id,
            va: b.va,
            w: b.w,
            h: b.h,
        })
    }

    /// Warp-4: an adoption half changed for surface `n` (glsrc write,
    /// present-to write, or either half's clean clear). If `n` is
    /// currently direct-scanned, route the source SWITCH through the
    /// uniform F16 pending rule (the resize-ack precedent): soft-Off --
    /// no device call, the old pixels persist -- and the next
    /// present-COMPLETE binds whatever source resolves then. The weave is
    /// marked stale in the DEACTIVATION direction by the callers that
    /// know it (GL frames never landed in it).
    fn gl_retarget(&mut self, n: usize) {
        if self.scanout == Scanout::Direct(n) {
            self.scanout = Scanout::Off;
            self.pending_direct = Some(n);
        }
    }

    /// Warp-4: a BO is dying (its own destroy, or its whole ctx's
    /// retire). If the DEVICE currently scans it out, rebind away FIRST
    /// -- an unref of the scanned-out resource is the one order the
    /// display cannot survive -- then re-arm the owning surface's own
    /// switch through reconcile (its next present restores the display
    /// from the weave, stale but bounded).
    fn gl_evict_res(&mut self, res_id: u32) {
        if res_id == 0 || self.bound_res != res_id {
            return;
        }
        let (dw, dh) = (self.gpu.width, self.gpu.height);
        let _ = self.gpu.set_scanout(0, dw, dh);
        self.bound_res = 0;
        if let Scanout::Direct(n) = self.scanout {
            self.scanout = Scanout::Off;
            self.pending_direct = Some(n);
            if let Some(s) = self.surf_mut(n) {
                s.res_stale = true;
            }
        } else {
            self.scanout = Scanout::Off;
        }
        self.reconcile();
    }

    /// Mint a context for `conn` (one per client -- the I-45 exposure
    /// bound): allocate the slot, CTX_CREATE on the device (virgl-gated by
    /// the caller), roll the slot back if the device refuses.
    fn wctx_mint(&mut self, conn: u64) -> Option<u32> {
        // Count RETIRING contexts too (round-3 F2): they are still this
        // conn's resources until they finish, and excluding them let one
        // connection mint-poison-destroy in a loop, burning a ctx slot
        // per abandoned fence.
        if self.warp_ctxs.iter().flatten().any(|c| c.owner_conn == conn) {
            return None; // one ctx per client
        }
        let slot = (0..MAX_WARP_CTXS)
            .find(|&i| self.warp_ctxs[i].is_none() && !self.warp_ctx_slot_poisoned[i])?;
        // Heap rows BEFORE any device state (#204): a failed allocation
        // fails the mint with nothing to unwind. The graveyard row is
        // reserved to the full cap here so `warp_park_leaked` -- which has
        // no failure arm by round-6 F1's argument -- never allocates. A
        // mintable slot's row is empty (parked records exist only while
        // the slot is poisoned, and poisoned slots are skipped above).
        let bos = warp_bo_row()?;
        // The row is empty (len 0), so this guarantees capacity >= the
        // full cap -- a no-op when a reused slot's row kept its capacity.
        debug_assert!(self.warp_ctx_leaked[slot].is_empty());
        // Same premise on the probe graveyard: a parked probe implies a
        // poisoned slot, and poisoned slots are skipped above.
        debug_assert!(self.warp_ctx_leaked_probe[slot].is_none());
        if self.warp_ctx_leaked[slot]
            .try_reserve_exact(MAX_WARP_BOS_PER_CTX)
            .is_err()
        {
            return None;
        }
        let dev_ctx = (slot as u32) + 1;
        if self.gpu.ctx_create(dev_ctx, b"warp").is_err() {
            return None;
        }
        // Never mint 0: it is `warp_ctx_vindicate`'s "no condemned slot"
        // sentinel, so a wrapped pub_id would make the vindication's
        // `position(|&p| p == 0)` match an arbitrary unrelated slot,
        // ctx_destroy a live host context and un-poison a legitimately
        // condemned one. Same 2^32 family as the deferred res_seq wrap, but
        // that one was dispositioned "verified to fail closed" -- this
        // sibling fails OPEN, and skipping the value costs nothing.
        self.warp_ctx_seq = self.warp_ctx_seq.wrapping_add(1);
        if self.warp_ctx_seq == 0 {
            self.warp_ctx_seq = 1;
        }
        let pub_id = self.warp_ctx_seq;
        self.warp_ctxs[slot] = Some(WarpCtx {
            owner_conn: conn,
            pub_id,
            dev_ctx,
            capset: 0,
            rings: 1,
            fences_in_flight: 0,
            fence_signaled: 0,
            fence_reported: 0,
            fenced_rx: 0,
            fenced_minted: 0,
            fenced_again: 0,
            fenced_err: 0,
            fence_poisoned: false,
            leaked_bytes: 0,
            leaked_count: 0,
            retiring: false,
            // #240: minted below, AFTER the ctx row exists. A probe that
            // cannot be built leaves `None` and every verify answers
            // "unknown" -- the ctx still works, it just cannot be asked.
            // Failing the whole ctx mint here would turn a diagnostic into
            // a new denial-of-service surface.
            probe: None,
            stream_rejected: false,
            rejected_at: 0,
            verify_seq: 0,
            verify_ok: 0,
            verify_tick: 0,
            verify_diag_arms: 0,
            verify_last: None,
            present_to: None,
            bo_backed_peak: 0,
            bo_bytes_peak: 0,
            build_diag_arms: 0,
            create_refused: 0,
            bos,
        });
        // #240: the probe is built AFTER the row exists so its failure is
        // recoverable -- a ctx with no probe still serves, it just answers
        // "unknown" to every verify. Making a diagnostic's failure fail the
        // whole mint would hand a client a new way to be denied a context.
        let probe = self.warp_probe_build(dev_ctx);
        if probe.is_none() {
            say!("tapestryd: warp ctx {} has no #240 health probe (mint failed)", pub_id);
        }
        if let Some(c) = self.wctx_mut(pub_id, conn) {
            c.probe = probe;
        }
        Some(pub_id)
    }

    /// Mint one 1x1 B8G8R8A8 render target owned by the SERVER, attached to
    /// `dev_ctx` but recorded nowhere a client resolve can reach. Returns
    /// `(res_id, fd, va)`. Every failure arm unwinds the device state in
    /// reverse and BOTH halves of the guest backing -- `t_burrow_detach`
    /// before `t_close`, since I-7's dual count frees nothing while a
    /// mapping ref survives.
    fn warp_probe_res(&mut self, dev_ctx: u32, size: u64) -> Option<(u32, i64, u64)> {
        let fd = unsafe { t_dma_create_gpu_bo(size, T_RIGHT_READ | T_RIGHT_WRITE | T_RIGHT_MAP) };
        if fd < 0 {
            return None;
        }
        let va = self.weave_va_next;
        self.weave_va_next += (size + PAGE - 1) & !(PAGE - 1);
        let pa = unsafe { t_dma_map(fd, va, T_PROT_READ | T_PROT_WRITE) };
        if pa < 0 {
            unsafe { t_close(fd) };
            return None;
        }
        self.res_seq = self.res_seq.wrapping_add(1);
        let res_id = self.res_seq;
        let undo = |gpu: &mut Gpu, stage: u32, res_id: u32| {
            if stage >= 2 {
                let _ = gpu.ctx_detach_resource(dev_ctx, res_id);
            }
            if stage >= 1 {
                let _ = gpu.resource_unref(res_id);
            }
            unsafe { t_burrow_detach(va, size) };
            unsafe { t_close(fd) };
        };
        if self
            .gpu
            .resource_create_3d(
                res_id,
                PIPE_TEXTURE_2D,
                VIRGL_FORMAT_B8G8R8A8_UNORM,
                VIRGL_BIND_RENDER_TARGET,
                1,
                1,
                1,
                1,
                0,
                0,
                0,
            )
            .is_err()
        {
            undo(&mut self.gpu, 0, res_id);
            return None;
        }
        if self.gpu.ctx_attach_resource(dev_ctx, res_id).is_err() {
            undo(&mut self.gpu, 1, res_id);
            return None;
        }
        if self.gpu.attach_backing(res_id, pa as u64, size as u32).is_err() {
            undo(&mut self.gpu, 2, res_id);
            return None;
        }
        Some((res_id, fd, va))
    }

    /// Release one built probe resource: device refs in reverse, then both
    /// halves of the guest backing -- `t_burrow_detach` before `t_close`,
    /// since I-7's dual count frees nothing while a mapping ref survives.
    /// ONE definition, used by every probe teardown (audit F4), because the
    /// bug it closed was a second copy of this sequence not existing.
    /// Split in two halves because the WEDGE posture defers exactly the
    /// second one (`wbo_retire`'s step 3): the device refs go on every
    /// path, the guest backing waits for the device-finished proof.
    fn warp_probe_res_undo(&mut self, dev_ctx: u32, res: u32, va: u64, fd: i64, size: u64) {
        self.warp_probe_undo_dev(dev_ctx, res);
        Self::warp_probe_undo_guest(va, fd, size);
    }

    fn warp_probe_undo_dev(&mut self, dev_ctx: u32, res: u32) {
        let _ = self.gpu.detach_backing(res);
        let _ = self.gpu.ctx_detach_resource(dev_ctx, res);
        let _ = self.gpu.resource_unref(res);
    }

    fn warp_probe_undo_guest(va: u64, fd: i64, size: u64) {
        unsafe { t_burrow_detach(va, size) };
        unsafe { t_close(fd) };
    }

    /// Build the #240 health probe for a freshly minted ctx (GPU-DESIGN
    /// 4.5.4b). `mark` is painted ONCE here by UPLOAD, never by a CLEAR:
    /// a CLEAR would need SET_FRAMEBUFFER_STATE, and virgl context state
    /// persists across command buffers while Mesa dirty-tracks its own
    /// binds -- so writing the mark that way would silently repoint a
    /// client's framebuffer at our resource.
    fn warp_probe_build(&mut self, dev_ctx: u32) -> Option<CtxProbe> {
        let size = PAGE;
        let (mark_res, mark_fd, mark_va) = self.warp_probe_res(dev_ctx, size)?;
        let (sent_res, sent_fd, sent_va) = match self.warp_probe_res(dev_ctx, size) {
            Some(v) => v,
            None => {
                self.warp_probe_res_undo(dev_ctx, mark_res, mark_va, mark_fd, size);
                return None;
            }
        };
        unsafe { core::ptr::write_volatile(mark_va as *mut u32, PROBE_MARK) };
        if self
            .gpu
            .transfer_to_3d_sync(dev_ctx, mark_res, 1, 1, 4)
            .is_err()
        {
            // The mark never reached the host, so a verify could not tell
            // "the copy ran" from "the copy ran and copied garbage".
            // Better no probe than a probe that answers wrongly.
            //
            // AUDIT F4: this arm returned with BOTH resources fully built
            // and unwound NOTHING -- two kernel handles, two mappings and
            // two device resources stranded per failed mint, and NOT even
            // reachable by the retire teardown, which is gated on the
            // `Some(p)` this arm never stores. The unwind lived inline in
            // the arm above, so the third arm silently had none: the reason
            // it is a shared helper now.
            self.warp_probe_res_undo(dev_ctx, sent_res, sent_va, sent_fd, size);
            self.warp_probe_res_undo(dev_ctx, mark_res, mark_va, mark_fd, size);
            return None;
        }
        Some(CtxProbe {
            mark_res,
            mark_fd,
            mark_va,
            sent_res,
            sent_fd,
            sent_va,
            size,
        })
    }

    /// One #240 health verify (GPU-DESIGN 4.5.4b): seed the sentinel with a
    /// token, ask the CONTEXT to copy the mark over it, read it back.
    ///
    /// The copy is `RESOURCE_COPY_REGION` -- stateless, so it cannot
    /// disturb whatever the client has bound. The seed and the readback are
    /// virtio-gpu commands rather than command-buffer ones, which is why
    /// they still work on a context vrend has latched off; that asymmetry
    /// IS the detector.
    ///
    /// Returns `Some(true)` healthy, `Some(false)` latched, `None` unknown.
    /// Every transport failure lands on `None`: an errored upload is not
    /// evidence of refusal, and must never read as health either.
    /// Audit F5/F2: ONE exit point folds every UNKNOWN into a comp-global
    /// count. Six `None` returns sit in the body below and a seventh is one
    /// edit away; counting at each site is the shape that grows a hole.
    /// The count is what makes UNKNOWN a state an operator can SEE at all --
    /// the per-arm `say!`s are now one-shot, so the rate has to live here.
    fn warp_ctx_verify(&mut self, ctx_pub: u32, conn: u64) -> Option<bool> {
        // ROUND-2 F9: a ctx that never HAD a probe is not an unknown
        // VERDICT, it is an unaskable question -- and counting it here let a
        // client drive the global rate counter at 9P-write rate, because the
        // per-ctx rate limit sits BELOW this arm and never damps it.
        // `verify-seq`/`verify-ok` already say "could not be asked" by
        // standing still.
        if self.wctx(ctx_pub, conn).map_or(true, |c| c.probe.is_none()) {
            return None;
        }
        // ROUND-3 F1 (and my own SF-3, found independently): count only when
        // a probe actually RAN. Round-2 F2 gave the rate-limit cache a
        // `None` return -- correctly, an UNKNOWN tick must not answer
        // "healthy" -- and this fold point counts on the return value alone,
        // so every re-verify inside that same tick bumped the counter having
        // done zero device I/O. That is round-2 F9's own defect one arm
        // over, recreated by F2 in the same commit, and it would pin a u32
        // at MAX and blind the only gauge carrying the UNKNOWN rate.
        // `verify_seq` advances iff the probe was admitted past the rate
        // limit, so it is the exact witness of "ran".
        let seq_before = self.wctx(ctx_pub, conn).map_or(0, |c| c.verify_seq);
        let r = self.warp_ctx_verify_probe(ctx_pub, conn);
        let ran = self
            .wctx(ctx_pub, conn)
            .map_or(false, |c| c.verify_seq != seq_before);
        if ran && r.is_none() {
            self.warp_verify_unknown = self.warp_verify_unknown.saturating_add(1);
        }
        if let Some(c) = self.wctx_mut(ctx_pub, conn) {
            c.verify_last = r;
        }
        r
    }

    /// Record an UNKNOWN on the console at most ONCE per arm per ctx.
    fn verify_unknown_once(&mut self, ctx_pub: u32, conn: u64, arm: u32) -> bool {
        match self.wctx_mut(ctx_pub, conn) {
            Some(c) if c.verify_diag_arms & (1 << arm) == 0 => {
                c.verify_diag_arms |= 1 << arm;
                true
            }
            _ => false,
        }
    }

    fn warp_ctx_verify_probe(&mut self, ctx_pub: u32, conn: u64) -> Option<bool> {
        let tick = self.tick;
        let (dev_ctx, mark_res, mark_va, sent_res, sent_va) = {
            let c = self.wctx(ctx_pub, conn)?;
            // One probe per ctx per compositor tick (see `verify_tick`). A
            // second write in the same frame is answered from the state the
            // first one established rather than re-running three synchronous
            // device round trips. `verify_seq` deliberately does NOT advance
            // here -- but note what it does and does NOT mean (audit F2): it
            // counts probes ADMITTED, incremented below BEFORE any device
            // I/O, so it advances on the UNKNOWN arms too. `verify_ok` is
            // the one that counts probes which reached a healthy VERDICT,
            // and it is what a reader must require to move before believing
            // a `stream-rejected 0`.
            //
            // AUDIT F12, for whoever writes the next test: under the
            // test-mode FROZEN clock the frame tick advances only on a
            // `tick` ctl write, so this rate limit pins every later verify
            // in the same frame to the cached answer -- a second probe
            // needs an intervening `tick`, not a sleep. (The reject
            // scenario runs with the clock LIVE, which is why its 100 ms
            // sleep genuinely buys a second probe.)
            // ROUND-2 F2: cache the VERDICT, never a proxy for it.
            // `!stream_rejected` reads TRUE (healthy) for a tick whose only
            // probe returned UNKNOWN -- because UNKNOWN correctly leaves
            // `stream_rejected` alone, and `verify_tick` is pinned before
            // any device I/O. That is round-1 F1's exact defect (UNKNOWN
            // silently becoming HEALTHY) surviving in the RETURN VALUE
            // after the round fixed it on the ctl. Three-valued in, three
            // valued out.
            if c.verify_tick == tick {
                if let Some(v) = c.verify_last {
                    return Some(v);
                }
                if c.verify_seq > 0 {
                    return None;
                }
            }
            let p = c.probe.as_ref()?;
            (c.dev_ctx, p.mark_res, p.mark_va, p.sent_res, p.sent_va)
        };
        if let Some(c) = self.wctx_mut(ctx_pub, conn) {
            c.verify_seq += 1;
            c.verify_tick = tick;
        }
        let seq = self.wctx(ctx_pub, conn).map_or(0, |c| c.verify_seq);

        // A token that differs from PROBE_MARK and from whatever the last
        // verify left, so "unchanged" is never satisfied by a stale value.
        //
        // SELF-AUDIT FINDING: the mix alone is not enough. `PROBE_TOKEN_BASE
        // ^ rot8(seq)` equals PROBE_MARK exactly when rot8(seq) is
        // 0x24443040 ^ 0x57415250, i.e. at seq 0x10730562 -- roughly 53 days
        // of 60 Hz verifying, which a long-lived compositor reaches. On that
        // one verify a LATCHED ctx would seed the mark's own value, read it
        // back, and be reported HEALTHY: a false negative on the exact
        // question this probe exists to answer. Perturb instead of trusting
        // the arithmetic.
        let mut token = PROBE_TOKEN_BASE ^ (seq as u32).rotate_left(8);
        if token == PROBE_MARK {
            token = !token;
        }
        // AUDIT F1, CONFIRMED BY MEASUREMENT: repaint the mark EVERY verify.
        // The probe's resources are attached to the CLIENT'S OWN dev_ctx, and
        // the submit stream is unparsed and carries raw device-global ids
        // that `bo/<id>/info` hands out from a shared counter -- so a client
        // derives `mark = first_res - 2` and copies over it. Measured on real
        // V3D: the mark read back as 0xFF00FF00, the client's own green.
        // Because `mark` used to be painted ONCE at create, that blinded the
        // detector for the ctx's whole life (every later verify -> UNKNOWN),
        // and UNKNOWN reads as healthy to anyone following the ctl. Repainting
        // makes the corruption last less than one verify: this upload, the
        // copy, and the readback all run inside a single dispatch, on one
        // in-order controlq, so no client submit can be published between
        // them. It is a virtio-gpu command, so it lands even on a latched ctx.
        unsafe { core::ptr::write_volatile(mark_va as *mut u32, PROBE_MARK) };
        if self
            .gpu
            .transfer_to_3d_sync(dev_ctx, mark_res, 1, 1, 4)
            .is_err()
        {
            return None;
        }
        unsafe { core::ptr::write_volatile(sent_va as *mut u32, token) };
        if self
            .gpu
            .transfer_to_3d_sync(dev_ctx, sent_res, 1, 1, 4)
            .is_err()
        {
            return None;
        }
        let mut st: [u32; 14] = [0; 14];
        st[0] = (VIRGL_CCMD_RESOURCE_COPY_REGION & 0xff) | (VIRGL_CMD_RCR_SIZE << 16);
        st[1] = sent_res; // dst handle
        st[2] = 0; // dst level
        st[3] = 0; // dst x
        st[4] = 0; // dst y
        st[5] = 0; // dst z
        st[6] = mark_res; // src handle
        st[7] = 0; // src level
        st[8] = 0; // src x
        st[9] = 0; // src y
        st[10] = 0; // src z
        st[11] = 1; // src w
        st[12] = 1; // src h
        st[13] = 1; // src d
        let mut bytes = [0u8; 56];
        for (i, w) in st.iter().enumerate() {
            bytes[i * 4..i * 4 + 4].copy_from_slice(&w.to_le_bytes());
        }
        if self.gpu.submit_3d_sync(dev_ctx, &bytes).is_err() {
            return None;
        }
        if self
            .gpu
            .transfer_from_3d_sync(dev_ctx, sent_res, 1, 1, 4)
            .is_err()
        {
            return None;
        }
        let got = unsafe { core::ptr::read_volatile(sent_va as *const u32) };
        // THE HEALTHY PATH IS SILENT AND COSTS NOTHING EXTRA. Reading back
        // PROBE_MARK is already proof that `mark` held PROBE_MARK -- the copy
        // is what delivered it -- so no separate mark check is needed here,
        // and no log line either: a `say!` per verify at the per-frame
        // cadence this verb is designed for would flood the console and
        // become its own performance defect.
        if got == PROBE_MARK {
            if let Some(c) = self.wctx_mut(ctx_pub, conn) {
                c.verify_ok += 1;
            }
            return Some(true);
        }
        // Everything below is about to make a SERIOUS claim ("this context is
        // dead"), so it pays for corroboration. Read the mark back: if it
        // does not hold PROBE_MARK then the upload/readback path is broken
        // and nothing about the copy can be concluded. A good mark plus an
        // unchanged sentinel is what genuinely means the copy did not run.
        let mark_now = {
            let mv = self
                .wctx(ctx_pub, conn)
                .and_then(|c| c.probe.as_ref())
                .map(|p| p.mark_va);
            match mv {
                Some(va) => {
                    if self.gpu.transfer_from_3d_sync(dev_ctx, mark_res, 1, 1, 4).is_err() {
                        None
                    } else {
                        Some(unsafe { core::ptr::read_volatile(va as *const u32) })
                    }
                }
                None => None,
            }
        };
        if mark_now != Some(PROBE_MARK) {
            if self.verify_unknown_once(ctx_pub, conn, 0) {
                say!(
                    "tapestryd: warp ctx {} verify {} -- the MARK reads {:?}, not {:#x}, so the \
                     probe cannot judge the copy. UNKNOWN, not a verdict. (first only; \
                     the rate is global `verify-unknown`)",
                    ctx_pub, seq, mark_now, PROBE_MARK
                );
            }
            return None;
        }
        if got != token {
            // Neither the mark nor our seed: the readback is not carrying
            // what this probe put there, so the probe itself is not
            // trustworthy on this ctx. Unknown, never a verdict.
            if self.verify_unknown_once(ctx_pub, conn, 1) {
                say!(
                    "tapestryd: warp ctx {} verify {} read {:#x} (neither mark nor token) \
                     -- unknown (first only; the rate is global `verify-unknown`)",
                    ctx_pub, seq, got
                );
            }
            return None;
        }
        if let Some(c) = self.wctx_mut(ctx_pub, conn) {
            if !c.stream_rejected {
                c.stream_rejected = true;
                c.rejected_at = seq;
                say!(
                    "tapestryd: warp ctx {} STREAM REJECTED -- the host refused a submit \
                     in (prev verify, {}]; the ctx is dead, recreate it (#240)",
                    ctx_pub, seq
                );
            }
        }
        Some(false)
    }

    /// Mint a BO record under a ctx (no device state yet -- the ctl
    /// `create3d` write allocates the backing + resource).
    fn wbo_mint(&mut self, ctx_pub: u32, conn: u64) -> Option<u32> {
        self.warp_bo_seq = self.warp_bo_seq.wrapping_add(1);
        let pub_id = self.warp_bo_seq;
        let c = self.wctx_mut(ctx_pub, conn)?;
        let slot = c.bos.iter().position(|b| b.is_none())?;
        c.bos[slot] = Some(WarpBo {
            pub_id,
            res_id: 0,
            dma_fd: -1,
            va: 0,
            pa: 0,
            size: 0,
            share_id: None,
            w: 0,
            h: 0,
            retiring: false,
        });
        Some(pub_id)
    }

    /// WDIAG_* arm indices for the create3d-refusal one-shot bitmasks.
    /// One bit per refusal family so every arm names itself exactly once
    /// per ctx (the per-ctx-once predecessor could name only ONE family
    /// per ctx lifetime, which blinded the #198 hunt to the second).
    const WDIAG_SIZE_ALIGN: u32 = 0;
    const WDIAG_SIZE_CAP: u32 = 1;
    const WDIAG_GEOMETRY: u32 = 2;
    const WDIAG_BYTE_CAP: u32 = 3;
    const WDIAG_COUNT_CAP: u32 = 4;
    const WDIAG_NO_MINT: u32 = 5;
    const WDIAG_DMA_CREATE: u32 = 6;
    const WDIAG_DMA_MAP: u32 = 7;
    const WDIAG_DEV_CREATE: u32 = 8;
    const WDIAG_DEV_ATTACH_CTX: u32 = 9;
    const WDIAG_DEV_ATTACH_BACKING: u32 = 10;
    const WDIAG_ALREADY_BUILT: u32 = 11;
    const WDIAG_CTX_GONE: u32 = 12;
    const WDIAG_CTL_PARSE: u32 = 13;
    const WDIAG_CTL_NO_RECORD: u32 = 14;
    const WDIAG_CTL_NOT_VIRGL: u32 = 15;
    const WDIAG_RECORD_VANISHED: u32 = 16;

    /// #218 one-shot diagnostic: the FIRST refused build per ctx names the
    /// failing arm + its parameters. Gated on the ctx latch, never the
    /// counting (#95), so a per-texture failure loop cannot storm the
    /// console it is diagnosing.
    #[allow(clippy::too_many_arguments)]
    fn wbo_diag_once(
        &mut self,
        ctx_pub: u32,
        conn: u64,
        arm: u32,
        why: &str,
        detail: i64,
        format: u32,
        w: u32,
        h: u32,
        last_level: u32,
        size: u64,
    ) {
        if let Some(c) = self.wctx_mut(ctx_pub, conn) {
            if c.build_diag_arms & (1 << arm) == 0 {
                c.build_diag_arms |= 1 << arm;
                say!(
                    "tapestryd: warp create3d refused ({} {}) fmt={} {}x{} lvl={} size={} ctx={}",
                    why, detail, format, w, h, last_level, size, ctx_pub
                );
            }
        } else {
            // A refusal whose ctx cannot be resolved must still be
            // nameable -- the per-ctx latch silently ate exactly this
            // class during the #198 hunt. Comp-level latch, same storm
            // bound.
            if self.warp_diag_noctx_arms & (1 << arm) == 0 {
                self.warp_diag_noctx_arms |= 1 << arm;
                say!(
                    "tapestryd: warp create3d refused ({} {}) fmt={} {}x{} lvl={} size={} ctx={} (UNRESOLVED)",
                    why, detail, format, w, h, last_level, size, ctx_pub
                );
            }
        }
    }

    /// The BO backing build (the ctl `create3d` verb): kernel GPU-BO mint
    /// -> map -> RESOURCE_CREATE_3D -> CTX_ATTACH -> ATTACH_BACKING, with
    /// reverse unwind on any failure. Size is the client's declared backing
    /// length; the kernel envelope + the client's own shared-map budget
    /// bound it -- the server checks only page alignment and non-zero.
    /// A `false` return leaves the mint record untouched HERE: the create3d
    /// ctl arm consumes it (#218), so every refusal family -- including the
    /// parse arms that never reach this function -- reclaims the slot at
    /// one chokepoint.
    #[allow(clippy::too_many_arguments)]
    fn wbo_create(
        &mut self,
        ctx_pub: u32,
        bo_pub: u32,
        conn: u64,
        target: u32,
        format: u32,
        bind: u32,
        w: u32,
        h: u32,
        d: u32,
        array: u32,
        last_level: u32,
        samples: u32,
        flags: u32,
        size: u64,
    ) -> bool {
        if size == 0 || size % PAGE != 0 {
            self.wbo_diag_once(ctx_pub, conn, Self::WDIAG_SIZE_ALIGN, "size-align", size as i64, format, w, h, last_level, size);
            return false;
        }
        // The backing is CLIENT-DECLARED, and nothing downstream charges
        // it: the kernel mint is tapestryd's (TCB, I-32-exempt) and the
        // per-Proc shared-map budget is charged only if the client ever
        // MAPS it. So the seam owns the bound (audit F2 -- the old
        // comment claimed the client's budget bounded this; it does not).
        // Two gates: the declared geometry must plausibly need this many
        // bytes (a 1x1 texture cannot ask for 64 MiB), and the ctx's
        // total live backing is capped.
        // Clamp against the ctx cap FIRST (round-2 F1 [P0]): every value
        // below is client-chosen, the release profile sets
        // overflow-checks = true + panic = "abort", and this Proc IS the
        // console -- so an unchecked add here is a remote kill, not a
        // wrong answer. Bounding `size` before the arithmetic means the
        // geometry math can never see a hostile magnitude, and every
        // step is checked anyway (belt AND braces: the round-1 version
        // checked only the multiplications and panicked on `v + v/2`).
        if size > WARP_CTX_BACKING_MAX {
            self.wbo_diag_once(ctx_pub, conn, Self::WDIAG_SIZE_CAP, "size-cap", 0, format, w, h, last_level, size);
            return false;
        }
        let px = (w as u64)
            .checked_mul(h.max(1) as u64)
            .and_then(|v| v.checked_mul(d.max(1) as u64))
            .and_then(|v| v.checked_mul(array.max(1) as u64));
        // Mip chains + alignment ride above the base level, so the slack
        // is generous; the point is only to refuse the absurd.
        let geom_max = match px
            .and_then(|v| v.checked_mul(WARP_BO_MAX_BPP))
            .and_then(|v| v.checked_add(v / 2))
            .and_then(|v| v.checked_add(PAGE))
        {
            Some(v) => v.max(PAGE),
            None => WARP_CTX_BACKING_MAX, // geometry alone is unbounded; the cap rules
        };
        if size > geom_max {
            self.wbo_diag_once(ctx_pub, conn, Self::WDIAG_GEOMETRY, "geometry", geom_max as i64, format, w, h, last_level, size);
            return false;
        }
        // The c-borrowing checks compute into locals first so the failure
        // arms can reach the (&mut self) one-shot diagnostic (#218).
        let (byte_fail, byte_live, count_fail, no_mint, already_built, dev_ctx) = {
            let c = match self.wctx(ctx_pub, conn) {
                Some(c) => c,
                None => {
                    self.wbo_diag_once(ctx_pub, conn, Self::WDIAG_CTX_GONE, "ctx-gone", 0, format, w, h, last_level, size);
                    return false;
                }
            };
            // Saturating, not `+` (round-2 F1's second witness): `live` sums
            // client-chosen sizes and the sum itself must never panic.
            let live: u64 =
                c.bos.iter().flatten().map(|b| b.size).fold(0u64, u64::saturating_add);
            let leaked = c.leaked_bytes;
        // Round-6 F1: the byte cap does NOT bound the leak COUNT. The
        // minimum accepted size is PAGE, so 16384 backings fit inside the
        // 64 MiB budget -- and since `bos[]` slots are reused across
        // mint/build/destroy, a poisoned-yet-live ctx could park all of
        // them into a 16-wide graveyard. The overflow then dropped each
        // surplus `WarpBo` by value, leaking a handle AND a mapping
        // (`WarpBo` has no `Drop`).
        //
        // The quantity that must be bounded is every backing this ctx will
        // EVER have to park, which is the ones already parked plus the ones
        // still live -- a live backing is parked wholesale by the leak arm
        // of `wctx_finish`. Bounding `leaked_count` alone would still admit
        // 15 parked + 16 live = 31 parks into a 16-wide graveyard. Only
        // BACKED BOs can be parked (`wbo_retire` returns 0, and so parks
        // nothing, when `dma_fd < 0`), so an unbacked mint is correctly not
        // counted here.
            let live_backed = c.bos.iter().flatten().filter(|b| b.dma_fd >= 0).count();
            (
                live.saturating_add(leaked).saturating_add(size) > WARP_CTX_BACKING_MAX,
                live,
                c.leaked_count as usize + live_backed >= MAX_WARP_BOS_PER_CTX,
                !c.bos.iter().flatten().any(|b| b.pub_id == bo_pub),
                c.bos
                    .iter()
                    .flatten()
                    .any(|b| b.pub_id == bo_pub && b.dma_fd >= 0),
                c.dev_ctx,
            )
        };
        if byte_fail {
            self.wbo_diag_once(ctx_pub, conn, Self::WDIAG_BYTE_CAP, "byte-cap", byte_live as i64, format, w, h, last_level, size);
            return false;
        }
        if count_fail {
            self.wbo_diag_once(ctx_pub, conn, Self::WDIAG_COUNT_CAP, "count-cap", 0, format, w, h, last_level, size);
            return false;
        }
        if no_mint {
            self.wbo_diag_once(ctx_pub, conn, Self::WDIAG_NO_MINT, "no-mint-record", 0, format, w, h, last_level, size);
            return false;
        }
        // Already built? create3d is once per BO -- benign, but SAY so:
        // this was the one silent refusal arm, and a silent arm is where
        // the #198 hunt lost a session to a contradiction it could not
        // name (client saw a refusal, server named nothing).
        if already_built {
            self.wbo_diag_once(ctx_pub, conn, Self::WDIAG_ALREADY_BUILT, "already-built", 0, format, w, h, last_level, size);
            return false;
        }

        let fd =
            unsafe { t_dma_create_gpu_bo(size, T_RIGHT_READ | T_RIGHT_WRITE | T_RIGHT_MAP) };
        if fd < 0 {
            self.wbo_diag_once(ctx_pub, conn, Self::WDIAG_DMA_CREATE, "dma-create", fd, format, w, h, last_level, size);
            return false;
        }
        let va = self.weave_va_next;
        self.weave_va_next += (size + PAGE - 1) & !(PAGE - 1);
        let pa = unsafe { t_dma_map(fd, va, T_PROT_READ | T_PROT_WRITE) };
        if pa < 0 {
            unsafe { t_close(fd) };
            self.wbo_diag_once(ctx_pub, conn, Self::WDIAG_DMA_MAP, "dma-map", pa, format, w, h, last_level, size);
            return false;
        }

        // Every failure arm from here unwinds the DEVICE state in reverse
        // AND both halves of the guest backing: `t_burrow_detach` before
        // `t_close`, because I-7's dual count frees nothing while a
        // mapping ref survives (audit F4 -- dropping only the handle
        // leaked up to 64 MiB of pinned contiguous pages for the life of
        // a PERSISTENT driver).
        let unwind = |gpu: &mut Gpu, stage: u32, res_id: u32| {
            if stage >= 3 {
                let _ = gpu.detach_backing(res_id);
            }
            if stage >= 2 {
                let _ = gpu.ctx_detach_resource(dev_ctx, res_id);
            }
            if stage >= 1 {
                let _ = gpu.resource_unref(res_id);
            }
            unsafe { t_burrow_detach(va, size) };
            unsafe { t_close(fd) };
        };

        self.res_seq = self.res_seq.wrapping_add(1);
        let res_id = self.res_seq;
        if self
            .gpu
            .resource_create_3d(res_id, target, format, bind, w, h, d, array, last_level, samples, flags)
            .is_err()
        {
            unwind(&mut self.gpu, 0, res_id);
            self.wbo_diag_once(ctx_pub, conn, Self::WDIAG_DEV_CREATE, "dev-create", target as i64, format, w, h, last_level, size);
            return false;
        }
        if self.gpu.ctx_attach_resource(dev_ctx, res_id).is_err() {
            unwind(&mut self.gpu, 1, res_id);
            self.wbo_diag_once(ctx_pub, conn, Self::WDIAG_DEV_ATTACH_CTX, "dev-attach-ctx", 0, format, w, h, last_level, size);
            return false;
        }
        if self.gpu.attach_backing(res_id, pa as u64, size as u32).is_err() {
            unwind(&mut self.gpu, 2, res_id);
            self.wbo_diag_once(ctx_pub, conn, Self::WDIAG_DEV_ATTACH_BACKING, "dev-attach-backing", 0, format, w, h, last_level, size);
            return false;
        }

        let c = self.wctx_mut(ctx_pub, conn).unwrap();
        let mut built = false;
        for b in c.bos.iter_mut().flatten() {
            if b.pub_id == bo_pub {
                b.res_id = res_id;
                b.dma_fd = fd;
                b.va = va;
                b.pa = pa as u64;
                b.size = size;
                b.w = w;
                b.h = h;
                built = true;
                break;
            }
        }
        if !built {
            // Unreachable (single-threaded dispatch; nothing between the
            // no_mint check and this write-back touches `bos`) -- but a
            // record that vanished must not strand the device state just
            // built for it, and must NAME itself (audit F2: this was the
            // one refusal arm still silent).
            unwind(&mut self.gpu, 3, res_id);
            self.wbo_diag_once(ctx_pub, conn, Self::WDIAG_RECORD_VANISHED, "record-vanished", 0, format, w, h, last_level, size);
            return false;
        }
        // #204 census: track the backed high-water on BOTH axes -- count
        // (what MAX_WARP_BOS_PER_CTX gates) and bytes (what
        // WARP_CTX_BACKING_MAX gates; bo-peak 26 with thousands of
        // refusals proved the byte axis can saturate at tiny counts).
        // Per-ctx AND global (the ctx's copy dies with it).
        let live = c.bos.iter().flatten().filter(|b| b.dma_fd >= 0).count() as u32;
        if live > c.bo_backed_peak {
            c.bo_backed_peak = live;
        }
        let live_bytes: u64 =
            c.bos.iter().flatten().map(|b| b.size).fold(0u64, u64::saturating_add);
        if live_bytes > c.bo_bytes_peak {
            c.bo_bytes_peak = live_bytes;
        }
        let (ctx_peak, ctx_bytes_peak) = (c.bo_backed_peak, c.bo_bytes_peak);
        if ctx_peak > self.warp_bo_peak {
            self.warp_bo_peak = ctx_peak;
        }
        if ctx_bytes_peak > self.warp_bo_bytes_peak {
            self.warp_bo_bytes_peak = ctx_bytes_peak;
        }
        true
    }

    /// #218 server half: remove a minted-but-unbuilt BO record whose
    /// create3d was refused -- the mint's exact inverse. Without it a
    /// per-texture failure loop filled `bos[]` with corpses and starved
    /// `wbo_mint` for the ctx's life (the #198 cascade's second stage).
    /// Bounded three ways: owner-conn (I-45 -- a foreign conn's failed
    /// write can never unmint another client's record), UNBUILT
    /// (`dma_fd < 0` -- a built BO is `wbo_destroy`'s to reclaim, so the
    /// benign already-built refusal cannot touch it), and non-retiring
    /// (a deferred destroy is the pump's). An unbacked record carries no
    /// bytes, no census, no graveyard charge: removal has no accounting
    /// to unwind.
    fn wbo_unmint_refused(&mut self, bo_pub: u32, conn: u64) {
        for c in self.warp_ctxs.iter_mut().flatten() {
            if c.owner_conn != conn || c.retiring {
                continue;
            }
            for slot in c.bos.iter_mut() {
                if slot
                    .as_ref()
                    .map_or(false, |b| b.pub_id == bo_pub && b.dma_fd < 0 && !b.retiring)
                {
                    *slot = None;
                    return;
                }
            }
        }
    }

    /// The BO Tweft mint (the weft_ensure precedent): share once, echo the
    /// stored id thereafter.
    fn wbo_weft_ensure(&mut self, bo_pub: u32, conn: u64) -> Option<(u64, u32)> {
        for c in self.warp_ctxs.iter_mut().flatten() {
            if c.owner_conn != conn || c.retiring {
                continue;
            }
            for b in c.bos.iter_mut().flatten() {
                if b.pub_id != bo_pub || b.retiring {
                    continue;
                }
                if b.dma_fd < 0 {
                    return None; // not built yet
                }
                if let Some(id) = b.share_id {
                    return Some((id, b.size as u32));
                }
                let id = unsafe { t_weft_share(b.va, b.size) };
                if id <= 0 {
                    say!("tapestryd: warp t_weft_share failed {}", id);
                    return None;
                }
                b.share_id = Some(id as u64);
                return Some((id as u64, b.size as u32));
            }
        }
        None
    }

    /// Retire one BO: the I-40/R2-F5 order -- (1) disarm the un-claimed
    /// share BEFORE any backing free (a Tweft claim racing the retire fails
    /// closed; an already-claimed share is a harmless miss and the CLIENT's
    /// mapping stays alive via the #847 dual count until its own teardown /
    /// the reaper); (2) device detach, under the drained-or-leaking
    /// precondition every caller establishes (W2d: each retire path drains
    /// the ctx's fences first, restoring the empty-in-flight state the
    /// Warp-2c synchrony gave by construction); (3) release the server's
    /// own refs -- UNLESS `leak`: a deadlined drain means an undrained
    /// fence may still DMA these pages, so the wedge posture pins them for
    /// the Proc's life (handle + mapping kept: leak-on-wedge, never UAF).
    /// Returns the byte count LEAKED (0 when the backing was freed) so the
    /// caller can keep charging it against the ctx cap (round-2 F3).
    fn wbo_retire(gpu: &mut Gpu, dev_ctx: u32, b: &mut WarpBo, leak: bool) -> u64 {
        if let Some(id) = b.share_id.take() {
            let _ = unsafe { t_weft_unshare(id) };
        }
        if b.dma_fd >= 0 {
            let _ = gpu.detach_backing(b.res_id);
            let _ = gpu.ctx_detach_resource(dev_ctx, b.res_id);
            let _ = gpu.resource_unref(b.res_id);
            // Capture BEFORE invalidating (round-3 F1 [P0]): round 2 moved
            // the `= -1` above the branch so the early return could observe
            // it, and left the close below reading the field -- t_close(-1)
            // on EVERY normal retire, so the kernel handle ref never
            // dropped and the whole 64 MiB backing stayed pinned for the
            // life of a persistent driver. The happy path leaked, unbounded
            // and uncharged, which is worse than the wedge leak the move
            // was made for.
            let fd = b.dma_fd;
            if leak {
                // `dma_fd` is deliberately LEFT VALID (round-5 F1): the
                // caller parks this record in its slot's graveyard, and a
                // later vindication -- the device proving it finished --
                // frees it from there. Zeroing it here would hand the
                // graveyard a record it could never free, which is the
                // round-3 F1 shape with the ownership reversed: there a
                // moved statement orphaned a use below it, here it would
                // orphan a use in another function.
                say!(
                    "tapestryd: warp bo res {} leak-parked {} bytes (fence wedge)",
                    b.res_id, b.size
                );
                return b.size;
            }
            b.dma_fd = -1;
            unsafe { t_burrow_detach(b.va, b.size) };
            unsafe { t_close(fd) };
        }
        0
    }

    /// Finish a ctx retire that is (or must be treated as) quiesced:
    /// every BO, then CTX_DESTROY (synchronous, so the device-side ctx is
    /// quiesced when the slot -- and with it the dev_ctx id -- becomes
    /// reusable). `leak` propagates the wedge posture to every backing.
    /// Park a backing the wedge posture refused to free, so the vindication
    /// can free it once the device proves it is finished (round-5 F1).
    ///
    /// INFALLIBLE by construction (round-6 F1), and that is load-bearing:
    /// the caller has already run `wbo_retire`, so there is no unwind left
    /// -- a failure here could only drop the record, and a dropped `WarpBo`
    /// leaks its still-valid `dma_fd` and its mapping with no `Drop` to
    /// catch it. The creation-time `leaked_count` cap admits at most one
    /// park per row entry, and the mint reserved the row to the full cap,
    /// so the guarded push below never allocates; the debug assert names
    /// that coupling rather than trusting it silently.
    fn warp_park_leaked(&mut self, slot: usize, b: WarpBo) {
        let g = &mut self.warp_ctx_leaked[slot];
        if g.len() < MAX_WARP_BOS_PER_CTX {
            g.push(b);
        } else {
            debug_assert!(false, "leak graveyard full: leaked_count cap breached");
            say!(
                "tapestryd: warp ctx slot {} leak graveyard full -- BUG, cap breached",
                slot
            );
        }
    }

    /// Free every backing parked for this slot. Sound ONLY at vindication:
    /// the device has just retired the chain that condemned them, which is
    /// the same proof that lets the slot itself be recycled.
    fn warp_free_leaked(&mut self, slot: usize) {
        // drain keeps the row's capacity, so a reused slot's mint-time
        // reserve stays a no-op.
        for b in self.warp_ctx_leaked[slot].drain(..) {
            if b.dma_fd >= 0 {
                unsafe { t_burrow_detach(b.va, b.size) };
                unsafe { t_close(b.dma_fd) };
            }
        }
        // The probe rides the same proof (#240 audit F3). Its device-side
        // refs went at the park -- as a BO's do -- so only the guest backing
        // was waiting on this.
        if let Some(p) = self.warp_ctx_leaked_probe[slot].take() {
            for (va, fd) in [(p.mark_va, p.mark_fd), (p.sent_va, p.sent_fd)] {
                Self::warp_probe_undo_guest(va, fd, p.size);
            }
            self.warp_probe_freed = self.warp_probe_freed.saturating_add(1);
        }
    }

    fn wctx_finish(&mut self, slot: usize, leak: bool) {
        if let Some(mut c) = self.warp_ctxs[slot].take() {
            // Take each backing by VALUE: a leak-parked record must outlive
            // this ctx (round-5 F1). Borrowing them, as this loop did, meant
            // the leaked bytes AND the records were dropped at the `return`
            // below -- and this was the one wbo_retire caller of three that
            // discarded the returned count, so the round-2 F3 accounting
            // died here with the only object holding it.
            for j in 0..MAX_WARP_BOS_PER_CTX {
                let mut b = match c.bos[j].take() {
                    Some(b) => b,
                    None => continue,
                };
                if Self::wbo_retire(&mut self.gpu, c.dev_ctx, &mut b, leak) > 0 {
                    self.warp_park_leaked(slot, b);
                }
            }
            // #240: the probe's two resources follow the SAME leak posture
            // as the BOs above -- which audit F3 caught this arm getting
            // WRONG IN BOTH HALVES. `wbo_retire` runs the device-side
            // unwind unconditionally and defers only step (3), the server's
            // own guest refs; this deferred BOTH and then parked NEITHER, so
            // a wedge stranded two kernel handles and two mappings for the
            // life of the Proc with no vindication able to reclaim them.
            // Now: device refs go on both postures, the guest backing waits
            // for the device-finished proof, and the unwind order is the
            // I-7 reverse either way (detach-before-close).
            if let Some(p) = c.probe.take() {
                for (res, va, fd) in [
                    (p.mark_res, p.mark_va, p.mark_fd),
                    (p.sent_res, p.sent_va, p.sent_fd),
                ] {
                    self.warp_probe_undo_dev(c.dev_ctx, res);
                    if !leak {
                        Self::warp_probe_undo_guest(va, fd, p.size);
                    }
                }
                if leak {
                    say!(
                        "tapestryd: warp ctx {} probe backings leak-parked ({} B, fence wedge)",
                        c.pub_id,
                        p.size * 2
                    );
                    // A RUNTIME guard, not a `debug_assert!` (round-2 F4):
                    // `[profile.release]` sets no `debug-assertions`, so
                    // every debug assert is compiled OUT of the shipped
                    // tapestryd -- an overwrite would silently strand two
                    // live fds and two mappings, `CtxProbe` having no
                    // `Drop`. The park-implies-poison chain says this cannot
                    // happen; `warp_park_leaked`'s overflow had exactly that
                    // status until round-6 F1 refuted it, and the answer
                    // then was a runtime guard too.
                    if self.warp_ctx_leaked_probe[slot].is_none() {
                        self.warp_ctx_leaked_probe[slot] = Some(p);
                        self.warp_probe_parked = self.warp_probe_parked.saturating_add(1);
                    } else {
                        // ROUND-3 F3: charge it anyway. The record IS
                        // permanently parked-without-freed -- which is
                        // exactly what this counter means -- and leaving it
                        // uncounted made `probe-parked - probe-freed` read
                        // BALANCED in the one case the guard exists to
                        // report. A ledger that goes quiet precisely when
                        // the BUG fires is the #184 shape.
                        self.warp_probe_parked = self.warp_probe_parked.saturating_add(1);
                        say!(
                            "tapestryd: warp ctx slot {} probe graveyard OCCUPIED -- BUG, \
                             park-implies-poison breached; the new record LEAKS rather than \
                             risk freeing pages the device may still hold (charged to \
                             probe-parked, which will never be matched by a free)",
                            slot
                        );
                    }
                }
            }
            if leak {
                // Round-2 F8: `leak` means "the device may still be
                // executing this ctx's stream". Freeing the slot would
                // hand its dev_ctx id to the NEXT client (dev_ctx =
                // slot+1), so a stale stream could execute against a
                // different client's context -- the I-45 breach F6
                // already closed one level down for fenced SLOTS and
                // that reasoning was not carried up to ctx slots.
                // Retire the slot instead, and do NOT destroy a context
                // with live work (that is what makes the host's
                // behaviour undefined in the first place).
                self.warp_ctx_slot_poisoned[slot] = true;
                self.warp_ctx_vindicate[slot] = c.pub_id;
                say!("tapestryd: warp ctx slot {} POISONED (fence wedge)", slot);
                return;
            }
            // A CLEAN finish is a stronger proof than a vindication: it
            // requires `fences_in_flight == 0` AND `!fence_poisoned`, and
            // the poison only clears via a vindication, which itself
            // requires the device to have retired every abandoned chain
            // this ctx owned. So anything parked earlier in this ctx's life
            // is provably free of the device now -- free it.
            //
            // Round 5 ALSO cleared an overflow-condemnation flag here, and
            // that was round-6 F1: it un-condemned a slot whose surplus
            // backings had been dropped and lost, handing the next ctx a
            // fresh 64 MiB budget with those pages gone -- the unbounded
            // cycle again, one abandoned fence per turn. The flag is gone
            // entirely now; the creation-time count cap means nothing is
            // ever dropped, so there is no condemnation left to clear and
            // no way for the ceiling to be re-armed.
            self.warp_free_leaked(slot);
            // Round-5 F3, the same shape at the clean-retire site: the ctx
            // was already taken above, so the slot -- and with it dev_ctx =
            // slot+1 -- is free the moment this returns. A refused destroy
            // means the host may still hold that context, so condemn the
            // slot rather than mint into a live id. No `warp_ctx_vindicate`
            // stamp: nothing can prove this one safe later, and a permanent
            // condemnation that `warp_poisoned_slots` reports is honest.
            if self.gpu.ctx_destroy(c.dev_ctx).is_err() {
                self.warp_ctx_slot_poisoned[slot] = true;
                say!(
                    "tapestryd: warp ctx slot {} destroy REFUSED on clean retire -- condemned",
                    slot
                );
            }
        }
    }

    /// Retire one ctx. Quiesced (no fences in flight) -> finish NOW, the
    /// common case. Otherwise mark it `retiring` -- instantly unresolvable
    /// to every client -- and let `warp_pump_retires` finish it when the
    /// last fence lands (audit F5: the old shape blocked the serve loop up
    /// to 2 s per object, and a client could multiply that into minutes of
    /// frozen console). Termination is the driver's: an unretired fence is
    /// abandoned within FENCE_ABANDON_MS, which decrements the counter and
    /// poisons the ctx, so `fences_in_flight` always reaches 0.
    fn wctx_retire(&mut self, slot: usize) {
        // Round-8 F1: release the hold HERE -- the one chokepoint every ctx
        // death passes through (the ctl `destroy` verb and conn teardown
        // both land here, and every `wctx_finish` is preceded by it). Round
        // 7 put it on `warp_retire_conn` only, which missed a client that
        // holds, submits, then `destroy`s its ctx while keeping the conn
        // open: the swallowed retire kept `fences_in_flight` nonzero, so the
        // pump could never finish the ctx it had just been told to retire.
        // Releasing before the quiesce test is what lets the deferred
        // retires replay in time for that test to succeed.
        #[cfg(feature = "test-mode")]
        if let Some(c) = self.warp_ctxs[slot].as_ref() {
            let pub_id = c.pub_id;
            self.gpu.test_hold_ctx_died(pub_id);
        }
        // Warp-4: the display must never keep scanning a resource this
        // retire will free (or leak-park) -- an unref of the scanned-out
        // resource is the one order the display cannot survive. Withdraw
        // the consent (the partner surface's next present re-routes
        // through its own 2D path, weave stale but bounded), then evict
        // any of this ctx's BOs from the device binding. Runs at THIS
        // chokepoint for the same reason the hold release does: every ctx
        // death passes through here, and the deferred finish only frees
        // what was already evicted now.
        let (evict_res, consent_sl) = self.warp_ctxs[slot].as_ref().map_or((None, None), |c| {
            (
                c.bos
                    .iter()
                    .flatten()
                    .map(|b| b.res_id)
                    .find(|&r| r != 0 && r == self.bound_res),
                c.present_to.map(|(sl, _, _)| sl),
            )
        });
        if let Some(c) = self.warp_ctxs[slot].as_mut() {
            c.present_to = None;
        }
        if let Some(sl) = consent_sl {
            if let Some(s) = self.surf_mut(sl) {
                s.res_stale = true;
            }
            self.gl_retarget(sl);
        }
        if let Some(r) = evict_res {
            self.gl_evict_res(r);
        }
        let (quiesced, poisoned) = match &self.warp_ctxs[slot] {
            Some(c) => (c.fences_in_flight == 0, c.fence_poisoned),
            None => return,
        };
        if quiesced {
            self.wctx_finish(slot, poisoned);
            return;
        }
        if let Some(c) = self.warp_ctxs[slot].as_mut() {
            c.retiring = true;
        }
    }

    /// The deferred-retire pump (audit F5), run per serve-loop pass after
    /// the fence drain: finish every ctx/BO whose fences have landed.
    fn warp_pump_retires(&mut self) {
        for i in 0..MAX_WARP_CTXS {
            let (quiesced, poisoned, ctx_retiring) = match &self.warp_ctxs[i] {
                Some(c) => (c.fences_in_flight == 0, c.fence_poisoned, c.retiring),
                None => continue,
            };
            if !quiesced {
                continue;
            }
            if ctx_retiring {
                self.wctx_finish(i, poisoned);
                continue;
            }
            let dev_ctx = self.warp_ctxs[i].as_ref().unwrap().dev_ctx;
            for j in 0..MAX_WARP_BOS_PER_CTX {
                let is_retiring = self.warp_ctxs[i].as_ref().unwrap().bos[j]
                    .as_ref()
                    .map_or(false, |b| b.retiring);
                if !is_retiring {
                    continue;
                }
                let mut b = self.warp_ctxs[i].as_mut().unwrap().bos[j].take().unwrap();
                let leaked = Self::wbo_retire(&mut self.gpu, dev_ctx, &mut b, poisoned);
                if leaked > 0 {
                    // Park it too (round-5 F1): `leaked_bytes` bounds this
                    // ctx's remaining life, but the ctx dies at wctx_finish
                    // and a client can cycle mint->leak->destroy, so the
                    // charge alone re-arms. The graveyard is what actually
                    // gets the pages back at vindication.
                    self.warp_park_leaked(i, b);
                }
                if let Some(c) = self.warp_ctxs[i].as_mut() {
                    c.leaked_bytes = c.leaked_bytes.saturating_add(leaked);
                    if leaked > 0 {
                        c.leaked_count = c.leaked_count.saturating_add(1);
                    }
                }
            }
        }
    }

    /// Retire a specific owned BO (the bo ctl `destroy` verb). An in-flight
    /// fenced chain may reference THIS BO (streams are scoped to
    /// ctx-attached resources), so a non-quiesced ctx defers: the BO
    /// becomes unresolvable immediately and the pump frees it.
    fn wbo_destroy(&mut self, bo_pub: u32, conn: u64) -> bool {
        let (ctx_pub, quiesced, poisoned) = match self.wbo(bo_pub, conn) {
            Some((c, _)) => (c.pub_id, c.fences_in_flight == 0, c.fence_poisoned),
            None => return false,
        };
        let slot = match self.wctx_slot(ctx_pub) {
            Some(s) => s,
            None => return false,
        };
        // Warp-4: same eviction contract as wctx_retire, scoped to one BO
        // -- both arms below make it unresolvable NOW (the deferred one
        // frees it later from the pump), so the display drops it now too.
        let (evict_res, consent_sl) = self.warp_ctxs[slot].as_ref().map_or((None, None), |c| {
            (
                c.bos
                    .iter()
                    .flatten()
                    .find(|b| b.pub_id == bo_pub)
                    .map(|b| b.res_id)
                    .filter(|&r| r != 0 && r == self.bound_res),
                match c.present_to {
                    Some((sl, _, bp)) if bp == bo_pub => Some(sl),
                    _ => None,
                },
            )
        });
        if let Some(sl) = consent_sl {
            if let Some(c) = self.warp_ctxs[slot].as_mut() {
                c.present_to = None;
            }
            if let Some(s) = self.surf_mut(sl) {
                s.res_stale = true;
            }
            self.gl_retarget(sl);
        }
        if let Some(r) = evict_res {
            self.gl_evict_res(r);
        }
        let dev_ctx = self.warp_ctxs[slot].as_ref().unwrap().dev_ctx;
        for j in 0..MAX_WARP_BOS_PER_CTX {
            let matches = self.warp_ctxs[slot].as_ref().unwrap().bos[j]
                .as_ref()
                .map_or(false, |b| b.pub_id == bo_pub);
            if !matches {
                continue;
            }
            if quiesced {
                let mut b = self.warp_ctxs[slot].as_mut().unwrap().bos[j].take().unwrap();
                let leaked = Self::wbo_retire(&mut self.gpu, dev_ctx, &mut b, poisoned);
                if leaked > 0 {
                    self.warp_park_leaked(slot, b);
                }
                if let Some(c) = self.warp_ctxs[slot].as_mut() {
                    c.leaked_bytes = c.leaked_bytes.saturating_add(leaked);
                    if leaked > 0 {
                        c.leaked_count = c.leaked_count.saturating_add(1);
                    }
                }
            } else {
                self.warp_ctxs[slot].as_mut().unwrap().bos[j]
                    .as_mut()
                    .unwrap()
                    .retiring = true;
            }
            return true;
        }
        false
    }

    /// Conn teardown, warp half: retire every ctx this conn owns.
    fn warp_retire_conn(&mut self, conn: u64) {
        // The hold release lives in `wctx_retire` (round-8 F1), which this
        // calls -- one chokepoint rather than three that must agree.
        for i in 0..MAX_WARP_CTXS {
            if self.warp_ctxs[i]
                .as_ref()
                .map_or(false, |c| c.owner_conn == conn)
            {
                self.wctx_retire(i);
            }
        }
    }

    /// Ctx slots condemned by a fence wedge (round-3 F7): a pool fully
    /// poisoned is a TERMINAL state, not a transient one, so it must be
    /// visible rather than inferable from a retryable-looking E_NOMEM.
    fn warp_poisoned_slots(&self) -> usize {
        self.warp_ctx_slot_poisoned.iter().filter(|&&p| p).count()
    }

    /// RESOLVABLE contexts -- retiring ones are excluded (round-2 F10).
    /// The `ctl` line and the arc's own gate assert on this, and a
    /// retiring ctx is addressable by nobody, so counting it made a
    /// correct system report a stale number (and the gate FAIL).
    fn warp_live_ctxs(&self) -> usize {
        self.warp_ctxs.iter().flatten().filter(|c| !c.retiring).count()
    }

    // --- Warp-2d: the fenced lane at the seam -----------------------------

    /// The per-pass fence pump: drain retired fenced chains off the device
    /// and post each on its owning ctx (poll_fences delivers them). A tag
    /// whose ctx is gone is dropped -- every retire path drains first, so
    /// only the wedge-leak path can orphan one.
    pub fn warp_service_fences(&mut self) {
        self.gpu.poll_completions();
        for tag in self.gpu.take_completions() {
            for c in self.warp_ctxs.iter_mut().flatten() {
                if c.pub_id != tag.ctx_pub {
                    continue;
                }
                c.fences_in_flight = c.fences_in_flight.saturating_sub(1);
                if tag.abandoned {
                    // NOT a completion: the chain may still be live
                    // device-side, so the ctx's backings can never be
                    // freed again (leak-on-wedge), and the fence must
                    // NOT be reported as signaled.
                    c.fence_poisoned = true;
                } else {
                    // #210 ROOT CAUSE FIX: count completions DENSELY per
                    // ctx instead of publishing the device-GLOBAL max
                    // fence id. The winsys counts fenced ops it ISSUED
                    // and compares against `fence-signaled`; with the
                    // global id, any ctx minted after prior fenced work
                    // (the SECOND GL client of a boot) saw signaled >>
                    // issued, and its unsigned in-flight throttle
                    // (issued - signaled) wrapped -- the client parked on
                    // the fence file forever while its next submit was
                    // the only thing that could fill the park. Dense
                    // per-ctx counts are the number space the client's
                    // model (and this file's own W2d comment) always
                    // assumed. Sound because completions are FIFO within
                    // the single ring; pairs 1:1 with the in_flight
                    // decrement above. The fence-file record's CONTENT
                    // moves to count-space too -- the winsys never parses
                    // it (the read is a doorbell; the counter is the
                    // authority).
                    c.fence_signaled += 1;
                }
                break;
            }
        }
        // A late retire proves the host finished an abandoned chain, so
        // its ctx (and the slot it may have condemned) recover -- without
        // this, one client could burn all 8 ctx slots in ~62 s and kill
        // the seam for the whole box permanently (round-3 F2).
        for v in self.gpu.take_vindications() {
            // #210 audit F1: a vindication IS a completion -- the device
            // provably finished one abandoned chain -- so the dense fence
            // count must take it, or the ctx is left one short of the
            // client's issue count forever (the silent post-recovery park:
            // signaled can never reach the parked wait's seq once nothing
            // later is in flight). Counted BEFORE the poison gate below:
            // take_vindications drains, and a vindication consumed while a
            // SIBLING slot is still poisoned would otherwise lose its
            // count permanently. The gate guards only the RECLAMATION
            // (un-poison + free), not the arithmetic.
            if let Some(c) = self
                .warp_ctxs
                .iter_mut()
                .flatten()
                .find(|c| c.pub_id == v.ctx_pub)
            {
                c.fence_signaled += 1;
            }
            // ONE retired chain is not proof for a ctx that abandoned
            // SEVERAL (round-4 F1): a ctx can hold every fenced slot, and
            // clearing its poison while siblings still execute would let
            // the next destroy FREE pages the device is still writing
            // (the UAF this posture exists to prevent) and recycle a
            // dev_ctx whose stream is live (the I-45 breach). Wait until
            // the driver holds no poisoned slot for this ctx at all.
            if self.gpu.ctx_has_poisoned_slot(v.ctx_pub) {
                continue;
            }
            // Un-poison a LIVE ctx, and make that a full reclamation point
            // (round-6 F1). The `ctx_has_poisoned_slot` test above is the
            // same device-done proof the slot recovery below relies on, so
            // this ctx's parked backings are provably free of the device
            // and can be freed HERE rather than waiting for it to die.
            // Both counters reset with them: an uncharge is honest exactly
            // when it is paired with the drop that FREES (the #130/#131
            // rule), and these pages are genuinely returned. Without this
            // a vindicated-but-still-live ctx stayed charged for memory it
            // no longer held, and once `leaked_count` hit the cap it could
            // never build another backing -- bricked while healthy.
            let live_slot = self
                .warp_ctxs
                .iter()
                .position(|c| c.as_ref().map_or(false, |c| c.pub_id == v.ctx_pub));
            if let Some(s) = live_slot {
                self.warp_free_leaked(s);
                if let Some(c) = self.warp_ctxs[s].as_mut() {
                    c.fence_poisoned = false;
                    c.leaked_bytes = 0;
                    c.leaked_count = 0;
                }
            }
            let slot = match self.warp_ctx_vindicate.iter().position(|&p| p == v.ctx_pub) {
                Some(s) => s,
                None => continue,
            };
            // The leak arm skipped CTX_DESTROY (a context with live work
            // must not be destroyed). Now that the device is provably
            // finished, destroy it BEFORE the slot -- and with it dev_ctx =
            // slot+1 -- returns to the pool, or the next client's
            // CTX_CREATE would collide with a host context that still
            // exists (round-4 F3). Round-5 F3: that was written as an
            // assertion, not a check -- `ctx_destroy` is fallible on a
            // HEALTHY engine (a resp-type mismatch does not latch `dead`),
            // and the un-poison ran regardless. A refused destroy leaves
            // the slot condemned instead.
            if self.gpu.ctx_destroy((slot as u32) + 1).is_err() {
                say!(
                    "tapestryd: warp ctx slot {} destroy REFUSED -- slot stays condemned",
                    slot
                );
                continue;
            }
            // Only now are the parked backings provably free of the device.
            self.warp_free_leaked(slot);
            self.warp_ctx_slot_poisoned[slot] = false;
            self.warp_ctx_vindicate[slot] = 0;
            say!("tapestryd: warp ctx slot {} recovered (device finished)", slot);
        }
        self.warp_pump_retires();
    }

    /// Does the serve loop need its 1 ms fence pace? A DEAD engine never
    /// retires anything, so counting its stuck slots would pin the clamp
    /// forever (self-found SF-1, same family as audit F6).
    pub fn warp_fences_pending(&self) -> bool {
        !self.gpu.engine_dead() && self.gpu.fenced_in_flight() > 0
    }

    /// SUBMIT_3D from the ctx submit file: one Twrite = one atomic opaque
    /// submission on the fenced lane. Returns the fence id (the fence
    /// file's completion record).
    /// Admission for every fenced submission, returning the device ctx id.
    ///
    /// Two bounds the lane itself cannot express. **Round-5 F2**: a poisoned
    /// ctx is TERMINAL -- the rest of the mechanism already treats its
    /// backings as possibly-live-DMA, so letting it submit again is what let
    /// a client re-arm its own fence stream and hide the wedge. **Round-5
    /// F4**: `alloc_fenced_slot` is first-fit over a process-wide pool of
    /// FENCED_SLOTS, and nothing capped a single ctx, so one client could
    /// take all four -- starving every other client, then poisoning all four
    /// at the abandonment deadline and killing 3D for the whole box. Half
    /// the lane is the share: it leaves room for a second client always, and
    /// still admits the submit+transfer pair a single client needs in
    /// flight together.
    fn warp_fenced_admit(&mut self, ctx_pub: u32, conn: u64) -> Result<u32, u32> {
        let c = self.wctx(ctx_pub, conn).ok_or(p9::E_NOENT)?;
        if c.fence_poisoned {
            return Err(E_IO);
        }
        if c.fences_in_flight as usize >= WARP_CTX_FENCE_MAX {
            return Err(E_AGAIN);
        }
        Ok(c.dev_ctx)
    }

    fn warp_submit(&mut self, ctx_pub: u32, conn: u64, stream: &[u8]) -> Result<u64, u32> {
        if let Some(c) = self.wctx_mut(ctx_pub, conn) {
            c.fenced_rx += 1;      // #210: arrived at the fenced funnel
        }
        let r = (|| {
            let dev_ctx = self.warp_fenced_admit(ctx_pub, conn)?;
            match self.gpu.submit_3d(dev_ctx, ctx_pub, stream) {
                Ok(f) => {
                    self.wctx_mut(ctx_pub, conn).unwrap().fences_in_flight += 1;
                    Ok(f)
                }
                Err(e) => Err(map_fenced_err(e)),
            }
        })();
        self.warp_fenced_account(ctx_pub, conn, &r);
        r
    }

    /// #210: classify one fenced-funnel outcome on the ctx ledger.
    fn warp_fenced_account(&mut self, ctx_pub: u32, conn: u64, r: &Result<u64, u32>) {
        if let Some(c) = self.wctx_mut(ctx_pub, conn) {
            match r {
                Ok(_) => c.fenced_minted += 1,
                Err(e) if *e == E_AGAIN => c.fenced_again += 1,
                Err(_) => c.fenced_err += 1,
            }
        }
    }

    /// TRANSFER_TO/FROM_HOST_3D from the bo ctl file (fence-bearing; the
    /// completion rides the owning ctx's fence file like a submit's).
    #[allow(clippy::too_many_arguments)]
    fn warp_transfer(
        &mut self,
        bo_pub: u32,
        conn: u64,
        to_host: bool,
        level: u32,
        x: u32,
        y: u32,
        z: u32,
        w: u32,
        h: u32,
        d: u32,
        offset: u64,
        stride: u32,
        layer_stride: u32,
    ) -> Result<u64, u32> {
        let (ctx_pub, res_id, built) = match self.wbo(bo_pub, conn) {
            Some((c, b)) => (c.pub_id, b.res_id, b.dma_fd >= 0),
            None => return Err(p9::E_NOENT),
        };
        if !built {
            return Err(p9::E_INVAL);
        }
        if let Some(c) = self.wctx_mut(ctx_pub, conn) {
            c.fenced_rx += 1;      // #210: arrived at the fenced funnel
        }
        let r = (|| {
            let dev_ctx = self.warp_fenced_admit(ctx_pub, conn)?;
            match self.gpu.transfer_3d(
                to_host, dev_ctx, ctx_pub, res_id, level, x, y, z, w, h, d, offset, stride,
                layer_stride,
            ) {
                Ok(f) => {
                    self.wctx_mut(ctx_pub, conn).unwrap().fences_in_flight += 1;
                    Ok(f)
                }
                Err(e) => Err(map_fenced_err(e)),
            }
        })();
        self.warp_fenced_account(ctx_pub, conn, &r);
        r
    }
}

fn map_fenced_err(e: FencedErr) -> u32 {
    match e {
        FencedErr::Again => E_AGAIN,
        FencedErr::TooBig => p9::E_INVAL,
        FencedErr::Dead => E_IO,
    }
}

pub struct Conn {
    handle: i64,
    pub conn_id: u64,
    /// Which tree this conn serves: P_ROOT (/srv/tapestry) or W_ROOT
    /// (/srv/warp) -- set by which listener accepted it (Warp-2c). One Conn
    /// type for both; the qid spaces are disjoint, so a warp conn simply
    /// never resolves a tapestry path and vice versa.
    root: u64,
    version_done: bool,
    msize: u32,
    fids: [Option<Fid>; MAX_FIDS],
    /// One-shot: the fid table filled (walks now refuse E_NOMEM). The #198
    /// hunt burned three instrumented runs because this refusal was the
    /// ONE silent layer between the client's diag and the warp dispatch's.
    fid_full_said: bool,
    in_buf: Vec<u8>,
    out_buf: Vec<u8>,
    defer: bool,
    pending_reads: Vec<PendingRead>,
    pending_fences: Vec<PendingFence>,
}

const NO_FID: Option<Fid> = None;

/// Exact union-cover test (#56): do `rects` (validated in-bounds,
/// nonempty) jointly cover the full (w, h) surface? The patchwork latch
/// must NOT trip on a full frame presented as TILES -- the battery's
/// G-6c multi-rect leg tiles the full surface in two halves by design
/// (the first-cut single-full-rect shortcut latched it -> the moveB
/// pane-center regression). Y-band sweep: for each horizontal band
/// between adjacent y-edges, the x-intervals of the rects spanning the
/// band must union to [0, w) gap-free. Exact for arbitrary overlap;
/// bounded by TPRESENT_MAX_RECTS=64 -> at most ~130 bands x 64
/// intervals per present -- negligible.
fn rects_cover_full(rects: &[(u32, u32, u32, u32)], w: u32, h: u32) -> bool {
    // Fast path: a single full-surface rect (the dominant shape --
    // rect_count 0, SDL_UpdateWindowSurface, present(None)).
    if rects.iter().any(|&(x, y, pw, ph)| x == 0 && y == 0 && pw == w && ph == h) {
        return true;
    }
    let mut ys: Vec<u32> = Vec::with_capacity(rects.len() * 2 + 2);
    ys.push(0);
    ys.push(h);
    for &(_, y, _, ph) in rects {
        ys.push(y);
        ys.push(y + ph);
    }
    ys.sort_unstable();
    ys.dedup();
    for win in ys.windows(2) {
        let (band_lo, band_hi) = (win[0], win[1]);
        // x-intervals of the rects fully spanning this band, sorted.
        let mut xs: Vec<(u32, u32)> = rects
            .iter()
            .filter(|&&(_, y, _, ph)| y <= band_lo && y + ph >= band_hi)
            .map(|&(x, _, pw, _)| (x, x + pw))
            .collect();
        xs.sort_unstable();
        let mut reach: u32 = 0;
        for (x0, x1) in xs {
            if x0 > reach {
                return false; // horizontal gap in this band
            }
            reach = reach.max(x1);
        }
        if reach < w {
            return false; // band not covered to the right edge
        }
    }
    true
}

impl Conn {
    pub fn new(handle: i64, conn_id: u64, root: u64) -> Conn {
        Conn {
            handle,
            conn_id,
            root,
            version_done: false,
            msize: SRV_MSIZE,
            fids: [NO_FID; MAX_FIDS],
            fid_full_said: false,
            in_buf: Vec::new(),
            out_buf: Vec::new(),
            defer: false,
            pending_reads: Vec::new(),
            pending_fences: Vec::new(),
        }
    }

    // --- fid table -----------------------------------------------------------

    fn fid_find(&self, fid: u32) -> Option<usize> {
        self.fids
            .iter()
            .position(|f| f.map_or(false, |f| f.fid == fid))
    }

    fn fid_set(&mut self, fid: u32, path: u64, gen: u32) -> bool {
        if let Some(i) = self.fid_find(fid) {
            self.fids[i] = Some(Fid {
                fid,
                path,
                gen,
                opened: false,
            });
            return true;
        }
        match self.fids.iter().position(|f| f.is_none()) {
            Some(i) => {
                self.fids[i] = Some(Fid {
                    fid,
                    path,
                    gen,
                    opened: false,
                });
                true
            }
            None => false,
        }
    }

    fn fid_full_diag_once(&mut self) {
        if !self.fid_full_said {
            self.fid_full_said = true;
            say!(
                "tapestryd: 9p fid table FULL (cap {}) conn={} -- walks refuse E_NOMEM",
                MAX_FIDS, self.conn_id
            );
        }
    }

    fn fid_clunk(&mut self, fid: u32) {
        if let Some(i) = self.fid_find(fid) {
            self.fids[i] = None;
        }
        // Cancel site 2 (clunk): this fid's held replies die with it.
        self.pending_reads.retain(|pr| pr.fid != fid);
        self.pending_fences.retain(|pf| pf.fid != fid);
    }

    fn drop_all_fids(&mut self, comp: &mut Comp) {
        // Cancel site 3 (Tversion, session reset): surfaces this conn owns
        // retire; every fid + held reply drops. Warp objects too.
        comp.retire_conn(self.conn_id);
        comp.warp_retire_conn(self.conn_id);
        self.fids = [NO_FID; MAX_FIDS];
        // A new session gets a new one-shot budget (fid-lift audit F5): the
        // reset empties the table, so a table that fills AGAIN deserves its
        // own witness -- a spent latch here silenced the second fill.
        self.fid_full_said = false;
        self.pending_reads.clear();
        self.pending_fences.clear();
    }

    pub fn teardown(&mut self, comp: &mut Comp) {
        // Cancel site 1 (conn death): the owning conn's surfaces retire
        // (spec Destroy via client death); held replies die. The warp half
        // retires the conn's GPU contexts + BOs identically (Warp-2c).
        comp.retire_conn(self.conn_id);
        comp.warp_retire_conn(self.conn_id);
        self.pending_reads.clear();
        self.pending_fences.clear();
    }

    pub fn raw_fd(&self) -> i64 {
        self.handle
    }

    /// Which tree this conn serves (the F7 per-root budget).
    pub fn root(&self) -> u64 {
        self.root
    }

    /// #210 custody mirror input: this conn's parked fence/event reads +
    /// request-buffer residue (a persistent nonzero in_buf between frames
    /// is a parse desync), plus the first parked fence's identity. Folded
    /// into Comp per tick by main's conns walk so the W_CTL reader (a
    /// sibling conn) can see it.
    #[cfg(feature = "test-mode")]
    pub fn w210_summary(&self) -> (usize, usize, usize, u32, u32) {
        let (fc, ff) = self
            .pending_fences
            .first()
            .map_or((0, 0), |p| (p.ctx_pub, p.fid));
        (
            self.pending_fences.len(),
            self.pending_reads.len(),
            self.in_buf.len(),
            fc,
            ff,
        )
    }

    // --- frame pump (the ptyfs bodies, verbatim shape) -----------------------

    pub fn service(&mut self, comp: &mut Comp) -> bool {
        let cur = self.in_buf.len();
        if cur >= SRV_MSIZE_USIZE {
            return false;
        }
        let want = SRV_MSIZE_USIZE - cur;
        self.in_buf.resize(cur + want, 0);
        let n =
            unsafe { libthyla_rs::t_read(self.handle, self.in_buf.as_mut_ptr().add(cur), want) };
        if n <= 0 {
            self.in_buf.truncate(cur);
            return false;
        }
        self.in_buf.truncate(cur + n as usize);

        loop {
            if self.in_buf.len() < p9::P9_HDR_LEN {
                return true;
            }
            let hdr = match p9::peek_header(&self.in_buf) {
                Ok(h) => h,
                Err(_) => return false,
            };
            let size = hdr.size as usize;
            if !(p9::P9_HDR_LEN..=SRV_MSIZE_USIZE).contains(&size) {
                return false;
            }
            if self.in_buf.len() < size {
                return true;
            }
            let frame: Vec<u8> = self.in_buf[..size].to_vec();
            match self.dispatch(comp, &frame, hdr) {
                Disp::Fatal => return false,
                Disp::Deferred => {}
                Disp::Reply(rlen) => {
                    if !self.send_all(rlen) {
                        return false;
                    }
                }
            }
            self.in_buf.drain(..size);
        }
    }

    fn dispatch(&mut self, comp: &mut Comp, tmsg: &[u8], hdr: p9::Header) -> Disp {
        let tag = hdr.tag;
        self.out_buf.clear();
        self.out_buf.resize(SRV_MSIZE_USIZE, 0);
        let r = match hdr.mtype {
            p9::P9_TVERSION => self.h_version(comp, tmsg, tag),
            p9::P9_TATTACH => self.h_attach(tmsg, tag),
            p9::P9_TWALK => self.h_walk(comp, tmsg, tag),
            p9::P9_TLOPEN => self.h_lopen(comp, tmsg, tag),
            p9::P9_TREAD => self.h_read(comp, tmsg, tag),
            p9::P9_TWRITE => self.h_write(comp, tmsg, tag),
            p9::P9_TREADDIR => self.h_readdir(comp, tmsg, tag),
            p9::P9_TGETATTR => self.h_getattr(comp, tmsg, tag),
            p9::P9_TCLUNK => self.h_clunk(tmsg, tag),
            p9::P9_TFLUSH => self.h_flush(tmsg, tag),
            p9::P9_TWEFT => self.h_weft(comp, tmsg, tag),
            _ => self.err(tag, p9::E_NOSYS),
        };
        if self.defer {
            self.defer = false;
            return Disp::Deferred;
        }
        let len = r.unwrap_or_else(|_| {
            self.out_buf.clear();
            self.out_buf.resize(SRV_MSIZE_USIZE, 0);
            p9::build_rlerror(&mut self.out_buf, tag, p9::E_PROTO).unwrap_or(0)
        });
        if len == 0 {
            Disp::Fatal
        } else {
            Disp::Reply(len)
        }
    }

    fn send_all(&mut self, rlen: usize) -> bool {
        let mut sent = 0usize;
        while sent < rlen {
            let w = unsafe {
                libthyla_rs::t_write(self.handle, self.out_buf.as_ptr().add(sent), rlen - sent)
            };
            if w <= 0 {
                return false;
            }
            sent += w as usize;
        }
        true
    }

    fn err(&mut self, tag: u16, code: u32) -> Result<usize, ()> {
        p9::build_rlerror(&mut self.out_buf, tag, code)
    }

    fn qid_of(&self, path: u64) -> p9::Qid {
        let kind = if is_dir(path) {
            p9::P9_QTDIR
        } else {
            p9::P9_QTFILE
        };
        p9::Qid {
            kind,
            version: 0,
            path,
        }
    }

    // --- handlers ------------------------------------------------------------

    fn h_version(&mut self, comp: &mut Comp, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_tversion(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        self.drop_all_fids(comp);
        self.msize = a.msize.min(SRV_MSIZE);
        self.version_done = true;
        p9::build_rversion(&mut self.out_buf, tag, self.msize, b"9P2000.L")
    }

    fn h_attach(&mut self, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        if !self.version_done {
            return self.err(tag, p9::E_PROTO);
        }
        let a = match p9::parse_tattach(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        if a.afid != p9::P9_NOFID || a.fid == p9::P9_NOFID {
            return self.err(tag, p9::E_OPNOTSUPP);
        }
        let root = self.root;
        if !self.fid_set(a.fid, root, 0) {
            self.fid_full_diag_once();
            return self.err(tag, p9::E_NOMEM);
        }
        let q = self.qid_of(root);
        p9::build_rattach(&mut self.out_buf, tag, &q)
    }

    /// Resolve one path component (the F2 ownership gate lives here).
    fn walk_child(&self, comp: &Comp, dir: u64, name: &[u8]) -> Option<(u64, u32)> {
        if name == b"." {
            return Some((dir, self.gen_for(comp, dir)));
        }
        match dir {
            P_ROOT => {
                if name == b".." {
                    Some((P_ROOT, 0))
                } else if name == b"ctl" {
                    Some((P_CTL, 0))
                } else if name == b"surface" {
                    Some((P_SURF_DIR, 0))
                } else if name == b"layout" {
                    Some((P_LAYOUT, 0))
                } else if name == b"pane" {
                    Some((P_PANE_DIR, 0))
                } else {
                    None
                }
            }
            P_PANE_DIR => {
                if name == b".." {
                    return Some((P_ROOT, 0));
                }
                let id = parse_u32(name)?;
                comp.layout.slot_of_id(id)?;
                Some((make_pane(id, PFK_DIR), 0))
            }
            d if is_pane(d) && pane_fk(d) == PFK_DIR => {
                let id = pane_id(d);
                comp.layout.slot_of_id(id)?;
                let fk = match name {
                    b".." => return Some((P_PANE_DIR, 0)),
                    b"ctl" => PFK_CTL,
                    b"mode" => PFK_MODE,
                    b"role" => PFK_ROLE,
                    b"tag" => PFK_TAG,
                    b"surface" => PFK_SURFACE,
                    b"geometry" => PFK_GEOMETRY,
                    _ => return None,
                };
                Some((make_pane(id, fk), 0))
            }
            P_SURF_DIR => {
                if name == b".." {
                    return Some((P_ROOT, 0));
                }
                if name == b"new" {
                    return Some((P_SURF_NEW, 0));
                }
                let n = parse_dec(name)?;
                let s = comp.surf(n)?;
                // F2: only the owning conn resolves a surface.
                if s.owner_conn != self.conn_id {
                    return None;
                }
                Some((make_surf(n, FK_DIR), s.gen))
            }
            d if is_surf(d) && surf_fk(d) == FK_DIR => {
                let n = surf_n(d);
                let s = comp.surf(n)?;
                if s.owner_conn != self.conn_id {
                    return None;
                }
                let fk = match name {
                    b".." => return Some((P_SURF_DIR, 0)),
                    b"ctl" => FK_CTL,
                    b"weave" => FK_WEAVE,
                    b"present" => FK_PRESENT,
                    b"event" => FK_EVENT,
                    b"geometry" => FK_GEOMETRY,
                    _ => return None,
                };
                Some((make_surf(n, fk), s.gen))
            }
            // --- The /dev/warp tree (Warp-2c) --------------------------------
            W_ROOT => match name {
                b".." => Some((W_ROOT, 0)),
                b"ctl" => Some((W_CTL, 0)),
                b"caps" => Some((W_CAPS, 0)),
                b"ctx" => Some((W_CTX_DIR, 0)),
                _ => None,
            },
            W_CTX_DIR => {
                if name == b".." {
                    return Some((W_ROOT, 0));
                }
                if name == b"new" {
                    return Some((W_CTX_NEW, 0));
                }
                let id = parse_u32(name)?;
                comp.wctx(id, self.conn_id)?; // F2: only the owner resolves
                Some((make_wctx(id, WFK_DIR), 0))
            }
            d if is_wctx(d) && warp_fk(d) == WFK_DIR => {
                let id = warp_id(d);
                comp.wctx(id, self.conn_id)?;
                let fk = match name {
                    b".." => return Some((W_CTX_DIR, 0)),
                    b"ctl" => WFK_CTL,
                    b"submit" => WFK_SUBMIT,
                    b"fence" => WFK_FENCE,
                    b"bo" => WFK_BO_DIR,
                    _ => return None,
                };
                Some((make_wctx(id, fk), 0))
            }
            d if is_wctx(d) && warp_fk(d) == WFK_BO_DIR => {
                let cid = warp_id(d);
                comp.wctx(cid, self.conn_id)?;
                if name == b".." {
                    return Some((make_wctx(cid, WFK_DIR), 0));
                }
                if name == b"new" {
                    return Some((make_wctx(cid, WFK_BO_NEW), 0));
                }
                let bid = parse_u32(name)?;
                let (c, _) = comp.wbo(bid, self.conn_id)?;
                if c.pub_id != cid {
                    return None;
                }
                Some((make_wbo(bid, WFK_DIR), 0))
            }
            d if is_wbo(d) && warp_fk(d) == WFK_DIR => {
                let bid = warp_id(d);
                let (c, _) = comp.wbo(bid, self.conn_id)?;
                let parent = make_wctx(c.pub_id, WFK_BO_DIR);
                let fk = match name {
                    b".." => return Some((parent, 0)),
                    b"ctl" => WFK_BO_CTL,
                    b"map" => WFK_BO_MAP,
                    b"info" => WFK_BO_INFO,
                    _ => return None,
                };
                Some((make_wbo(bid, fk), 0))
            }
            _ => None,
        }
    }

    fn gen_for(&self, comp: &Comp, path: u64) -> u32 {
        if is_surf(path) {
            comp.surf(surf_n(path)).map_or(0, |s| s.gen)
        } else {
            0
        }
    }

    fn h_walk(&mut self, comp: &mut Comp, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_twalk(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let i = match self.fid_find(a.fid) {
            Some(i) => i,
            None => return self.err(tag, p9::E_BADF),
        };
        let f = self.fids[i].unwrap();
        if f.opened {
            return self.err(tag, p9::E_PROTO);
        }
        let mut cur = f.path;
        let mut cur_gen = f.gen;
        let mut qids: [p9::Qid; p9::P9_MAX_WALK] = [p9::Qid::default(); p9::P9_MAX_WALK];
        let mut nwalked = 0usize;
        for k in 0..(a.nwname as usize) {
            match self.walk_child(comp, cur, a.names[k]) {
                Some((next, gen)) => {
                    cur = next;
                    cur_gen = gen;
                    qids[nwalked] = self.qid_of(next);
                    nwalked += 1;
                }
                None => break,
            }
        }
        if nwalked == a.nwname as usize {
            if !self.fid_set(a.newfid, cur, cur_gen) {
                self.fid_full_diag_once();
                return self.err(tag, p9::E_NOMEM);
            }
        } else if nwalked == 0 && a.nwname > 0 {
            return self.err(tag, p9::E_NOENT);
        }
        p9::build_rwalk(&mut self.out_buf, tag, &qids[..nwalked])
    }

    fn h_lopen(&mut self, comp: &mut Comp, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_tlopen(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let i = match self.fid_find(a.fid) {
            Some(i) => i,
            None => return self.err(tag, p9::E_BADF),
        };
        let f = self.fids[i].unwrap();
        if f.opened {
            return self.err(tag, p9::E_PROTO);
        }

        // The mint idiom (netd clone / ptyfs ptmx): opening surface/new
        // allocates a surface in THIS conn and rebinds the fid onto its ctl.
        if f.path == P_SURF_NEW {
            if comp.owned_count(self.conn_id) >= MAX_SURFACES_PER_CONN {
                return self.err(tag, p9::E_NOMEM); // F9 per-conn cap
            }
            let conn_id = self.conn_id;
            let n = match comp.mint(conn_id) {
                Some(n) => n,
                None => return self.err(tag, p9::E_NOMEM),
            };
            let gen = comp.surf(n).unwrap().gen;
            let path = make_surf(n, FK_CTL);
            self.fids[i] = Some(Fid {
                fid: a.fid,
                path,
                gen,
                opened: true,
            });
            let q = self.qid_of(path);
            return p9::build_rlopen(&mut self.out_buf, tag, &q, 0);
        }

        // Warp mints (Warp-2c). ctx/new: allocate a context in THIS conn
        // (CTX_CREATE on the device -- virgl-gated) + rebind the fid onto
        // its ctl; the ctl read yields the public id. bo/new: the same
        // idiom one level down (no device state until the ctl create3d).
        if f.path == W_CTX_NEW {
            if !comp.gpu.virgl {
                return self.err(tag, p9::E_OPNOTSUPP);
            }
            let conn_id = self.conn_id;
            let id = match comp.wctx_mint(conn_id) {
                Some(id) => id,
                None => return self.err(tag, p9::E_NOMEM),
            };
            let path = make_wctx(id, WFK_CTL);
            self.fids[i] = Some(Fid { fid: a.fid, path, gen: 0, opened: true });
            let q = self.qid_of(path);
            return p9::build_rlopen(&mut self.out_buf, tag, &q, 0);
        }
        if is_wctx(f.path) && warp_fk(f.path) == WFK_BO_NEW {
            let cid = warp_id(f.path);
            let conn_id = self.conn_id;
            if comp.wctx(cid, conn_id).is_none() {
                return self.err(tag, p9::E_NOENT);
            }
            let id = match comp.wbo_mint(cid, conn_id) {
                Some(id) => id,
                None => return self.err(tag, p9::E_NOMEM),
            };
            let path = make_wbo(id, WFK_BO_CTL);
            self.fids[i] = Some(Fid { fid: a.fid, path, gen: 0, opened: true });
            let q = self.qid_of(path);
            return p9::build_rlopen(&mut self.out_buf, tag, &q, 0);
        }
        // Warp files: re-validate liveness + ownership (monotonic ids -- a
        // stale fid resolves to nothing, the pane discipline).
        if is_wctx(f.path)
            && warp_fk(f.path) != WFK_DIR
            && comp.wctx(warp_id(f.path), self.conn_id).is_none()
        {
            return self.err(tag, p9::E_NOENT);
        }
        if is_wbo(f.path) && comp.wbo(warp_id(f.path), self.conn_id).is_none() {
            return self.err(tag, p9::E_NOENT);
        }

        // Surface files: re-validate liveness + ownership + generation (a
        // walk could have raced a retire).
        if is_surf(f.path) && !comp.surf_owned(surf_n(f.path), self.conn_id, f.gen) {
            return self.err(tag, p9::E_NOENT);
        }
        // Pane files: re-validate liveness (ids are never reused, so a
        // stale fid can only resolve to nothing).
        if is_pane(f.path) && comp.layout.slot_of_id(pane_id(f.path)).is_none() {
            return self.err(tag, p9::E_NOENT);
        }
        self.fids[i].as_mut().unwrap().opened = true;
        let q = self.qid_of(f.path);
        p9::build_rlopen(&mut self.out_buf, tag, &q, 0)
    }

    fn h_read(&mut self, comp: &mut Comp, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_tread(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let i = match self.fid_find(a.fid) {
            Some(i) => i,
            None => return self.err(tag, p9::E_BADF),
        };
        let f = self.fids[i].unwrap();
        if !f.opened || is_dir(f.path) {
            return self.err(tag, p9::E_INVAL);
        }
        let cap = ((self.msize as usize).saturating_sub(p9::P9_HDR_LEN + 4)).min(a.count as usize);

        if f.path == P_CTL {
            // `display W H` first: the line a client parses to size a
            // fullscreen create (placement stays hidden -- D5 -- but the
            // DISPLAY geometry is global, not placement).
            let mut s = String::new();
            let _ = core::fmt::write(
                &mut s,
                format_args!(
                    "display {} {}\nsurfaces {}\nclock-rate {}\ntick {}\npanes {}\nfocused {}\n",
                    comp.gpu.width,
                    comp.gpu.height,
                    comp.live_count(),
                    comp.clock_hz,
                    comp.tick,
                    comp.layout.live_ids().len(),
                    comp.layout.id_of(comp.layout.focused).unwrap_or(0)
                ),
            );
            #[cfg(feature = "test-mode")]
            {
                let _ = core::fmt::write(
                    &mut s,
                    format_args!("test-mode {}\n", if comp.test_mode { "on" } else { "off" }),
                );
            }
            return self.read_str(tag, &s, a.offset, cap);
        }
        if f.path == P_LAYOUT {
            // The container tree as text (G-6). Reads regenerate the
            // string; a multi-read straddling a mutation can tear -- the
            // text fits one frame at every realistic size (the stage-0
            // ctl posture).
            let s = comp.layout.render_text();
            return self.read_str(tag, &s, a.offset, cap);
        }
        if is_pane(f.path) {
            return self.pane_read(comp, f.path, tag, a.offset, cap);
        }

        // --- The /dev/warp files (Warp-2c) -----------------------------------
        if f.path == W_CTL {
            // `virgl <0|1>` first: the line joey's boot probe + a client's
            // capability check parse. capset names the RETAINED blob (the
            // caps file's content); ctxs is live-count introspection.
            let mut s = String::new();
            let _ = core::fmt::write(
                &mut s,
                format_args!(
                    "virgl {}\ncapsets {}\ncapset {} {} {}\nctxs {}\npoisoned {}\nbo-cap {}\nfence-lane {}\nbo-peak {}\n",
                    comp.gpu.virgl as u32,
                    comp.gpu.num_capsets,
                    comp.gpu.capset_id,
                    comp.gpu.capset_ver,
                    comp.gpu.capset_blob.len(),
                    comp.warp_live_ctxs(),
                    comp.warp_poisoned_slots(),
                    // Warp-3: client discovery of the per-ctx BO capacity, so
                    // neither the winsys nor the prover's churn bound bakes
                    // the constant in (#187: a claimed cross-file sync needs
                    // a CHECK; reading it here IS the check).
                    MAX_WARP_BOS_PER_CTX,
                    // #204: the per-ctx fenced-lane share, for the same
                    // reason -- the winsys throttles its in-flight depth to
                    // this, and a hardcoded client mirror is what held the
                    // pipeline at depth 2 (#215) after the server side grew.
                    WARP_CTX_FENCE_MAX,
                    // #204 census (global): max backed BOs any ctx ever
                    // held -- readable after the workload's ctx is gone.
                    comp.warp_bo_peak
                ),
            );
            // The BYTES axis (what WARP_CTX_BACKING_MAX actually gates).
            let _ = core::fmt::write(
                &mut s,
                format_args!("bo-bytes-peak {}\n", comp.warp_bo_bytes_peak),
            );
            // #240 audit F3: the probe-graveyard ledger. NOT test-mode --
            // parked-without-freed is the shape of a real handle leak in
            // the process that IS the console, so it must be readable on a
            // production build, exactly like the BO leak counters beside it.
            let _ = core::fmt::write(
                &mut s,
                format_args!(
                    "probe-parked {}\nprobe-freed {}\nverify-unknown {}\n",
                    comp.warp_probe_parked, comp.warp_probe_freed, comp.warp_verify_unknown
                ),
            );
            // Round-3 F4: where the FIXED-size prefix ends. Re-set below in
            // the test-mode block, whose `w210` line is also fixed size.
            let mut gcrit_end = s.len();
            // Audit F3/F4: the ctx-less refusal ledger + the comp-global
            // one-shot mask -- both are spendable/bumpable by ANY conn, so
            // both must be READABLE or their state is invisible exactly
            // when the hunt needs it.
            let _ = core::fmt::write(
                &mut s,
                format_args!(
                    "create-refused-noctx {}\ndiag-noctx-arms {}\n",
                    comp.warp_create_refused_noctx, comp.warp_diag_noctx_arms
                ),
            );
            // #175: the anti-vacuous counter. A prover that submits and
            // then abandons is racing the drain; if the completion won,
            // nothing was in flight, the abandon was a no-op, and every
            // later assertion runs against a HEALTHY ctx while reporting
            // PASS. Asserting `abandoned >= 1` first is what makes the
            // poisoned-path legs mean anything.
            #[cfg(feature = "test-mode")]
            {
                let _ = core::fmt::write(
                    &mut s,
                    format_args!("abandoned {}\n", comp.gpu.test_abandoned_total()),
                );
                // #180: the departed-holder discriminator. `ctxs` cannot serve
                // here -- warp_live_ctxs excludes `retiring` contexts, so a ctx
                // whose deferred retires were never replayed (round-8 F1) reads
                // exactly like one that finished. This counts the slots
                // themselves, which is the resource that actually leaks.
                let _ = core::fmt::write(
                    &mut s,
                    format_args!("fenced-free {}\n", comp.gpu.test_fenced_free()),
                );
                // #210: the per-ctx fence ledger, readable from ANY conn. The
                // signaled/reported pair splits a wedged fence wait three
                // ways: reported == signaled == the id a parked client waits
                // past means the newest record was CONSUMED by a read (an
                // Rread was sent -- a client still parked lost it in
                // transport); reported < signaled means an undelivered
                // record sits here while the park never fills (a poll_fences
                // gap); inflight > 0 belies fenced-free.
                // #210 custody: reads the server HOLDS right now (folded
                // per tick across ALL conns) + parse residue. fparked=0
                // with a client-side parked Tread on a fence fid means
                // the read never became a park -- look at inbuf.
                //
                // BEFORE the per-ctx loop (round-2 F6), not after: this
                // line is FIXED size while that loop grows ~66 bytes per
                // live ctx, and the in-tree reader takes a 512-byte
                // snapshot. Behind it, the custody mirror silently vanished
                // from about four live ctxs -- going blind exactly when a
                // multi-client hunt needs it. The unbounded thing goes last.
                let _ = core::fmt::write(
                    &mut s,
                    format_args!(
                        "w210 fparked {} rparked {} inbuf {} fctx {} ffid {}\n",
                        comp.w210_fparked,
                        comp.w210_rparked,
                        comp.w210_inbuf_max,
                        comp.w210_f_ctx,
                        comp.w210_f_fid
                    ),
                );
                // The fixed-size prefix ends HERE; everything below grows
                // with the live-ctx count and is deliberately non-critical.
                gcrit_end = s.len();
                for c in comp.warp_ctxs.iter().flatten() {
                    let _ = core::fmt::write(
                        &mut s,
                        format_args!(
                            "wctx {} inflight {} signaled {} reported {} rx {} mint {} again {} err {}\n",
                            c.pub_id,
                            c.fences_in_flight,
                            c.fence_signaled,
                            c.fence_reported,
                            c.fenced_rx,
                            c.fenced_minted,
                            c.fenced_again,
                            c.fenced_err
                        ),
                    );
                }
            }
            // ROUND-3 F4: the per-ctx ctl got a runtime width report and its
            // global sibling -- widened by six lines in the same arc -- got
            // only an ordering convention. Same check, same reason: a
            // truncation is silent, `parse_field` simply misses the key and
            // the reader takes a default. Measured through `w210` (the last
            // FIXED-size line; the per-ctx `wctx` rows after it are
            // deliberately unbounded and non-critical).
            if gcrit_end > W_CTL_SNAPSHOT && !comp.warp_gctl_wide_said {
                comp.warp_gctl_wide_said = true;
                say!(
                    "tapestryd: warp global ctl fixed prefix is {} bytes (file {}), past the \
                     {}-byte client snapshot -- keys after the cut are INVISIBLE to a short \
                     reader",
                    gcrit_end,
                    s.len(),
                    W_CTL_SNAPSHOT
                );
            }
            return self.read_str(tag, &s, a.offset, cap);
        }
        if f.path == W_CAPS {
            // The raw capset blob (decode with the ctl `capset` line).
            let b = &comp.gpu.capset_blob;
            let off = (a.offset as usize).min(b.len());
            let take = (b.len() - off).min(cap);
            let slice = b[off..off + take].to_vec();
            return p9::build_rread(&mut self.out_buf, tag, &slice);
        }
        if is_wctx(f.path) {
            let id = warp_id(f.path);
            if comp.wctx(id, self.conn_id).is_none() {
                return self.err(tag, p9::E_NOENT);
            }
            return match warp_fk(f.path) {
                WFK_CTL => {
                    let mut s = String::new();
                    // Where the keys a CLIENT parses end (round-2 F5); set
                    // after the fences block below and checked at the tail.
                    let mut crit_end = 0usize;
                    let _ = core::fmt::write(&mut s, format_args!("{}\n", id));
                    // #175: expose the per-ctx leak accounting so the
                    // prover can watch the round-6 F1 cap directly rather
                    // than inferring it from a refusal alone.
                    if let Some(c) = comp.wctx(id, self.conn_id) {
                        let _ = core::fmt::write(
                            &mut s,
                            format_args!(
                                "poisoned {}\nleaked-count {}\nleaked-bytes {}\n",
                                c.fence_poisoned as u32, c.leaked_count, c.leaked_bytes
                            ),
                        );
                        // #240 (GPU-DESIGN 4.5.4b). DISTINCT from `poisoned`
                        // on purpose: that one means a fence chain never
                        // retired, this one means the host refused our
                        // commands while every fence retired normally. The
                        // defect existed because the second was read through
                        // the first. `verify-seq` rides along so a reader
                        // can tell "never asked" (0) from "asked, healthy"
                        // -- a zero `stream-rejected` alone is satisfied by
                        // a probe that never ran (#184).
                        let _ = core::fmt::write(
                            &mut s,
                            format_args!(
                                "stream-rejected {}\nrejected-at {}\nverify-seq {}\nverify-ok {}\n",
                                c.stream_rejected as u32, c.rejected_at, c.verify_seq, c.verify_ok
                            ),
                        );
                    }
                    // #180: a BOUNDED way to watch this ctx's fences land. The
                    // fence fd is the only alternative and it parks, so a
                    // broken per-ctx hold scope would hang a prover instead of
                    // failing it -- and a hang reads as a boot timeout, which
                    // is the least diagnosable failure this harness can emit.
                    // `fence-signaled` rides alongside because the gauge alone
                    // cannot carry the held-lane claim (#184): it is satisfied
                    // by "no fence was ever queued" just as well as by "the
                    // fence retired". `fence_signaled` is monotonic, and a
                    // SWALLOWED retire never reaches the pump that raises it,
                    // so an increase is positive evidence a fence really landed.
                    //
                    // PROMOTED out of test-mode at Warp-3: the Mesa winsys's
                    // whole fence model rides `fence-signaled` (the client
                    // counts fenced ops it ISSUED; the fence file coalesces,
                    // so this monotonic counter is the only raceless way to
                    // learn how many RETIRED -- the same #184 reasoning that
                    // put it here). A production client cannot depend on a
                    // test-mode field.
                    {
                        let signaled =
                            comp.wctx(id, self.conn_id).map_or(0, |c| c.fence_signaled);
                        let _ = core::fmt::write(
                            &mut s,
                            format_args!(
                                "fences-in-flight {}\nfence-signaled {}\n",
                                comp.gpu.ctx_fences_in_flight(id),
                                signaled
                            ),
                        );
                        // Everything the client PARSES ends here (round-2
                        // F5 + the self-found half): the winsys reads this
                        // file into a 256-byte buffer and pulls
                        // `fence-signaled` out of it, so the guard below
                        // must measure THIS prefix, not the whole file.
                        // Measuring the whole file would cry wolf -- with
                        // realistic 10-digit monotonic counters the tail
                        // (`create-refused`) genuinely passes 255 on a
                        // long-uptime box while every client-critical key
                        // is still comfortably inside.
                        crit_end = s.len();
                        // #204 census (per-ctx): live = backed right now
                        // (what the creation cap counts); peak = the
                        // high-water the cap width must be sized against;
                        // the bytes twins gate WARP_CTX_BACKING_MAX.
                        let (live, peak, live_b, peak_b) =
                            comp.wctx(id, self.conn_id).map_or((0, 0, 0, 0), |c| {
                                (
                                    c.bos
                                        .iter()
                                        .flatten()
                                        .filter(|b| b.dma_fd >= 0)
                                        .count() as u32,
                                    c.bo_backed_peak,
                                    c.bos
                                        .iter()
                                        .flatten()
                                        .map(|b| b.size)
                                        .fold(0u64, u64::saturating_add),
                                    c.bo_bytes_peak,
                                )
                            });
                        let _ = core::fmt::write(
                            &mut s,
                            format_args!(
                                "bo-live {}\nbo-peak {}\nbo-bytes {}\nbo-bytes-peak {}\n",
                                live, peak, live_b, peak_b
                            ),
                        );
                        // The #198-hunt storm scale: the one-shots name only
                        // the FIRST refusal per family; this counts them all.
                        // Appended LAST so the client-critical keys
                        // (fence-signaled) stay inside a 255-byte snapshot.
                        let refused =
                            comp.wctx(id, self.conn_id).map_or(0, |c| c.create_refused);
                        let _ = core::fmt::write(
                            &mut s,
                            format_args!("create-refused {}\n", refused),
                        );
                    }
                    // Audit F11: the "keep the client-critical keys inside a
                    // 255-byte snapshot" discipline above was a comment with
                    // nothing checking it, and the C-0d block was inserted
                    // mid-file against it. A truncation fails SILENTLY
                    // toward a smaller value (the #210 silent-park shape:
                    // `parse_field` simply does not find the key and the
                    // client reads a default), which is the failure mode
                    // that must never be discovered by its consequences.
                    // One-shot, because it would otherwise fire per read.
                    //
                    // Round-1 claimed a "~219 worst case, 36 bytes of
                    // margin"; round-2 F5 refuted the figure and a
                    // recomputation puts the client-critical prefix at ~192
                    // and the WHOLE file at ~285 under reachable magnitudes
                    // (10-digit monotonic counters; caps of 1024 BOs and
                    // 64 MiB). Do not re-derive a margin from that prose --
                    // this runtime check is the only authority, which is why
                    // it measures rather than asserts.
                    if crit_end > WCTX_CTL_SNAPSHOT && !comp.warp_ctl_wide_said {
                        comp.warp_ctl_wide_said = true;
                        say!(
                            "tapestryd: warp ctx ctl client-critical prefix is {} bytes (file \
                             {}), past the {}-byte client snapshot -- keys after the cut are \
                             INVISIBLE to a short reader",
                            crit_end,
                            s.len(),
                            WCTX_CTL_SNAPSHOT
                        );
                    }
                    self.read_str(tag, &s, a.offset, cap)
                }
                // The fence completion stream (W2d): one record per read
                // -- the newest completion COUNT (#210: dense per-ctx,
                // not the device fence id), coalescing (FIFO within our
                // single ring, so count N retires everything <= N); parks
                // when nothing is unreported (the FK_EVENT netd leg).
                // Offset is ignored: a stream, not a file image.
                WFK_FENCE => {
                    if cap < FENCE_REC_MAX {
                        // Too small for a whole record: answer empty rather
                        // than park a read that could never complete.
                        return p9::build_rread(&mut self.out_buf, tag, &[]);
                    }
                    let c = comp.wctx(id, self.conn_id).unwrap();
                    if c.fence_poisoned {
                        // Stream over: a fence of this ctx was abandoned
                        // and can never signal (round-2 F7). UNCONDITIONAL
                        // since round-5 F2 -- the old `&& fence_signaled <=
                        // fence_reported` conjunct let the client suppress
                        // its own EOF: fence ids are globally monotone, so
                        // ANY later submission that completes leaves
                        // signaled > reported, and the read then returned
                        // that higher id. Under the documented "id N
                        // retires everything <= N" coalescing rule that
                        // record ASSERTS the abandoned fence completed --
                        // so the client would reuse a backing this very
                        // seam believes the device may still be writing.
                        // A vindication clears the poison and the stream
                        // legitimately resumes.
                        return p9::build_rread(&mut self.out_buf, tag, &[]);
                    }
                    if c.fence_signaled > c.fence_reported {
                        let v = c.fence_signaled;
                        comp.wctx_mut(id, self.conn_id).unwrap().fence_reported = v;
                        let mut s = String::new();
                        let _ = core::fmt::write(&mut s, format_args!("{}\n", v));
                        return p9::build_rread(&mut self.out_buf, tag, s.as_bytes());
                    }
                    if self.pending_fences.len() >= MAX_FIDS {
                        return self.err(tag, p9::E_PROTO);
                    }
                    self.pending_fences.push(PendingFence {
                        fid: a.fid,
                        ctx_pub: id,
                        tag,
                    });
                    self.defer = true;
                    Ok(0)
                }
                _ => self.err(tag, p9::E_INVAL),
            };
        }
        if is_wbo(f.path) {
            let id = warp_id(f.path);
            let bo = match comp.wbo(id, self.conn_id) {
                Some((_, b)) => b,
                None => return self.err(tag, p9::E_NOENT),
            };
            return match warp_fk(f.path) {
                WFK_BO_CTL => {
                    let mut s = String::new();
                    let _ = core::fmt::write(&mut s, format_args!("{}\n", id));
                    self.read_str(tag, &s, a.offset, cap)
                }
                WFK_BO_INFO => {
                    // `res` is the device-global resource id -- the name a
                    // virgl command stream uses (the gpu_va slot of the
                    // section 4.1 tree is v3d's; virgl addresses by id).
                    // stride 0 = tight (the client picks per-transfer).
                    let mut s = String::new();
                    let _ = core::fmt::write(
                        &mut s,
                        format_args!(
                            "res {} size {} stride 0 offset 0 w {} h {}\n",
                            bo.res_id, bo.size, bo.w, bo.h
                        ),
                    );
                    self.read_str(tag, &s, a.offset, cap)
                }
                _ => self.err(tag, p9::E_INVAL),
            };
        }

        if !is_surf(f.path) {
            return self.err(tag, p9::E_INVAL);
        }
        let n = surf_n(f.path);
        let fk = surf_fk(f.path);

        // The event stream outlives its surface by exactly one EOF: a
        // retired surface's event fid reads empty (stream end).
        if !comp.surf_owned(n, self.conn_id, f.gen) {
            if fk == FK_EVENT {
                return p9::build_rread(&mut self.out_buf, tag, &[]);
            }
            return self.err(tag, p9::E_NOENT);
        }

        match fk {
            FK_CTL => {
                // The netd clone idiom: the ctl read returns the surface id.
                let mut s = String::new();
                let _ = core::fmt::write(&mut s, format_args!("{}\n", n));
                self.read_str(tag, &s, a.offset, cap)
            }
            FK_WEAVE => {
                let surf = comp.surf(n).unwrap();
                if surf.state == SurfState::Minted {
                    return self.err(tag, p9::E_INVAL);
                }
                let mut s = String::new();
                let _ = core::fmt::write(
                    &mut s,
                    format_args!(
                        "{} {} {} {} {} b8g8r8a8\n",
                        surf.w,
                        surf.h,
                        surf.w * 4,
                        surf.slot_stride,
                        WEAVE_SLOTS
                    ),
                );
                self.read_str(tag, &s, a.offset, cap)
            }
            FK_GEOMETRY => {
                let surf = comp.surf(n).unwrap();
                let mut s = String::new();
                let _ = core::fmt::write(
                    &mut s,
                    format_args!("0 0 {} {} 0 0\n", surf.w, surf.h),
                );
                self.read_str(tag, &s, a.offset, cap)
            }
            FK_EVENT => {
                if cap < TEVENT_LEN {
                    // Too small for even one record: answer empty rather
                    // than park a read that could never complete.
                    return p9::build_rread(&mut self.out_buf, tag, &[]);
                }
                if let Some(len) = self.drain_events(comp, n, cap) {
                    let data: Vec<u8> = self.scratch_events(comp, n, cap, len);
                    return p9::build_rread(&mut self.out_buf, tag, &data);
                }
                // Empty: park (the netd WouldBlock leg).
                if self.pending_reads.len() >= MAX_FIDS {
                    return self.err(tag, p9::E_PROTO);
                }
                self.pending_reads.push(PendingRead {
                    fid: a.fid,
                    surf: n,
                    gen: f.gen,
                    tag,
                    cap,
                });
                self.defer = true;
                Ok(0)
            }
            _ => self.err(tag, p9::E_INVAL),
        }
    }

    /// How many whole event records are deliverable now (None = zero).
    fn drain_events(&self, comp: &Comp, n: usize, cap: usize) -> Option<usize> {
        let s = comp.surf(n)?;
        if s.events.is_empty() {
            return None;
        }
        Some((cap / TEVENT_LEN).min(s.events.len()))
    }

    fn scratch_events(&self, comp: &mut Comp, n: usize, _cap: usize, count: usize) -> Vec<u8> {
        let mut data = alloc::vec![0u8; count * TEVENT_LEN];
        if let Some(s) = comp.surf_mut(n) {
            for k in 0..count {
                let ev = s.events.pop_front().unwrap();
                ev.encode(&mut data[k * TEVENT_LEN..(k + 1) * TEVENT_LEN]);
            }
        }
        data
    }

    fn read_str(&mut self, tag: u16, s: &str, offset: u64, cap: usize) -> Result<usize, ()> {
        let b = s.as_bytes();
        let off = (offset as usize).min(b.len());
        let take = (b.len() - off).min(cap);
        let slice = b[off..off + take].to_vec();
        p9::build_rread(&mut self.out_buf, tag, &slice)
    }

    /// Pane file reads (G-6). A stale id (the pane closed) is E_NOENT --
    /// ids are never reused, so there is nothing it could alias.
    fn pane_read(
        &mut self,
        comp: &Comp,
        path: u64,
        tag: u16,
        offset: u64,
        cap: usize,
    ) -> Result<usize, ()> {
        let id = pane_id(path);
        let slot = match comp.layout.slot_of_id(id) {
            Some(s) => s,
            None => return self.err(tag, p9::E_NOENT),
        };
        let mut s = String::new();
        match pane_fk(path) {
            PFK_CTL => {
                let _ = core::fmt::write(&mut s, format_args!("{}\n", id));
            }
            PFK_MODE => {
                let name = match comp.layout.get(slot).map(|p| &p.kind) {
                    Some(pane::Kind::Container { mode, .. }) => mode.name(),
                    _ => "leaf",
                };
                let _ = core::fmt::write(&mut s, format_args!("{}\n", name));
            }
            PFK_ROLE => {
                let p = comp.layout.get(slot).unwrap();
                let _ = core::fmt::write(
                    &mut s,
                    format_args!(
                        "{} {}\n",
                        p.role.name(),
                        if p.focusable { "focusable" } else { "nofocus" }
                    ),
                );
            }
            PFK_TAG => {
                let p = comp.layout.get(slot).unwrap();
                let _ = core::fmt::write(&mut s, format_args!("{}\n", p.tag));
            }
            PFK_SURFACE => match comp.layout.leaf_surface(slot) {
                Some(n) => {
                    let _ = core::fmt::write(&mut s, format_args!("{}\n", n));
                }
                None => s.push_str("none\n"),
            },
            PFK_GEOMETRY => {
                let c = comp.layout.get(slot).unwrap().content;
                let _ = core::fmt::write(
                    &mut s,
                    format_args!("{} {} {} {}\n", c.x, c.y, c.w, c.h),
                );
            }
            _ => return self.err(tag, p9::E_INVAL),
        }
        self.read_str(tag, &s, offset, cap)
    }

    /// Pane file writes (G-6): the per-pane ctl carries the layout verbs
    /// with the fid's pane as the implicit target; mode/role/tag are
    /// direct field writes.
    fn pane_write(&mut self, comp: &mut Comp, path: u64, data: &[u8]) -> Result<(), u32> {
        let s = core::str::from_utf8(data).map_err(|_| p9::E_INVAL)?;
        let s = s.trim();
        let id = pane_id(path);
        match pane_fk(path) {
            PFK_CTL => comp.pane_cmd(id, s),
            PFK_MODE => {
                let mode = Mode::parse(s).ok_or(p9::E_INVAL)?;
                let slot = comp.layout.slot_of_id(id).ok_or(p9::E_NOENT)?;
                if !comp.layout.set_mode(slot, mode) {
                    return Err(p9::E_INVAL);
                }
                comp.reconcile();
                Ok(())
            }
            PFK_TAG => {
                let slot = comp.layout.slot_of_id(id).ok_or(p9::E_NOENT)?;
                comp.layout.get_mut(slot).unwrap().tag = String::from(s);
                Ok(())
            }
            PFK_ROLE => {
                let slot = comp.layout.slot_of_id(id).ok_or(p9::E_NOENT)?;
                let mut it = s.split_ascii_whitespace();
                let role = Role::parse(it.next().ok_or(p9::E_INVAL)?).ok_or(p9::E_INVAL)?;
                let focusable = match it.next() {
                    None => true,
                    Some("focusable") => true,
                    Some("nofocus") => false,
                    Some(_) => return Err(p9::E_INVAL),
                };
                if it.next().is_some() {
                    return Err(p9::E_INVAL);
                }
                let p = comp.layout.get_mut(slot).unwrap();
                p.role = role;
                p.focusable = focusable;
                Ok(())
            }
            _ => Err(p9::E_PERM),
        }
    }

    fn h_write(&mut self, comp: &mut Comp, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_twrite(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let i = match self.fid_find(a.fid) {
            Some(i) => i,
            None => return self.err(tag, p9::E_BADF),
        };
        let f = self.fids[i].unwrap();
        if !f.opened || is_dir(f.path) {
            return self.err(tag, p9::E_INVAL);
        }

        if f.path == P_CTL {
            return match self.global_ctl(comp, a.data) {
                Ok(()) => p9::build_rwrite(&mut self.out_buf, tag, a.count),
                Err(e) => self.err(tag, e),
            };
        }
        if f.path == P_LAYOUT {
            return match core::str::from_utf8(a.data)
                .map_err(|_| p9::E_INVAL)
                .and_then(|s| comp.layout_cmd(s))
            {
                Ok(()) => p9::build_rwrite(&mut self.out_buf, tag, a.count),
                Err(e) => self.err(tag, e),
            };
        }
        if is_pane(f.path) {
            return match self.pane_write(comp, f.path, a.data) {
                Ok(()) => p9::build_rwrite(&mut self.out_buf, tag, a.count),
                Err(e) => self.err(tag, e),
            };
        }
        // --- The /dev/warp writes (Warp-2c) ----------------------------------
        // #175: the warp-tree test levers must live HERE, not on the
        // tapestry P_CTL where the other test-mode verbs are. A warp client
        // is connected to /srv/warp and has no path to the tapestry tree at
        // all, so verbs parked there were simply unreachable -- the gate
        // caught that on its first real run, which is the harness earning
        // its keep before it ever tested the seam. The cargo feature IS the
        // production strip (#880).
        #[cfg(feature = "test-mode")]
        if f.path == W_CTL {
            let s = core::str::from_utf8(a.data).unwrap_or("").trim();
            if s == "warp-hold on" || s == "warp-hold off" || s == "warp-abandon" {
                // #178: both levers act ONLY on the caller's own ctx. They
                // ship (`default = ["test-mode"]`) and this ctl is mode
                // 0666, so global versions were an unprivileged, permanent,
                // box-wide DoS on the fenced lane -- handing any client the
                // very wedge 149-warp.md documents as needing a hung GL
                // chain. Authorizing the CALLER cannot work here: the
                // in-guest battery is an ordinary uid-1000 client by design
                // (the same reason SA-1 leaves the determinism surface
                // ungated), so the power is bounded instead of the peer.
                // Requiring a ctx also makes a mis-sequenced test fail loud
                // rather than silently no-op.
                let ctx_pub = match comp.wctx_of_conn(self.conn_id) {
                    Some(v) => v,
                    None => return self.err(tag, p9::E_INVAL),
                };
                if s == "warp-abandon" {
                    comp.gpu.test_abandon_ctx(ctx_pub);
                    return p9::build_rwrite(&mut self.out_buf, tag, a.count);
                }
                // Round-8 F1: at most ONE ctx may hold. Arming used to
                // overwrite `hold_ctx` outright, so a second client
                // displaced the first WITHOUT replaying its swallowed
                // retires -- and the displaced ctx then never quiesced,
                // stranding its fenced slot; four of those exhaust the
                // shared lane for every client. Refusing the displacement
                // keeps the departing-holder release sufficient by
                // construction. A non-holder's "off" is likewise refused
                // rather than silently releasing someone else's hold.
                let held = comp.gpu.test_hold_ctx_current();
                if s.ends_with("on") {
                    if held.map_or(false, |h| h != ctx_pub) {
                        return self.err(tag, p9::E_AGAIN);
                    }
                    comp.gpu.test_hold_ctx(Some(ctx_pub));
                } else {
                    if held.map_or(false, |h| h != ctx_pub) {
                        return self.err(tag, p9::E_AGAIN);
                    }
                    comp.gpu.test_hold_ctx(None);
                }
                return p9::build_rwrite(&mut self.out_buf, tag, a.count);
            }
            return self.err(tag, p9::E_INVAL);
        }
        if is_wctx(f.path) {
            let id = warp_id(f.path);
            if comp.wctx(id, self.conn_id).is_none() {
                return self.err(tag, p9::E_NOENT);
            }
            return match warp_fk(f.path) {
                WFK_CTL => match self.wctx_ctl(comp, id, a.data) {
                    Ok(()) => p9::build_rwrite(&mut self.out_buf, tag, a.count),
                    Err(e) => self.err(tag, e),
                },
                // One Twrite = one atomic submission (W2d): the opaque
                // stream (section 2.1 -- unparsed) rides the fenced lane;
                // the reply means QUEUED, the ctx fence file carries the
                // completion. Bounded by iounit; the Loom-carried bulk
                // path is the section 4.1 follow-on.
                WFK_SUBMIT => {
                    if !comp.gpu.virgl {
                        return self.err(tag, p9::E_OPNOTSUPP);
                    }
                    match comp.warp_submit(id, self.conn_id, a.data) {
                        Ok(_fence) => p9::build_rwrite(&mut self.out_buf, tag, a.count),
                        Err(e) => self.err(tag, e),
                    }
                }
                _ => self.err(tag, p9::E_PERM),
            };
        }
        if is_wbo(f.path) {
            let id = warp_id(f.path);
            if comp.wbo(id, self.conn_id).is_none() {
                return self.err(tag, p9::E_NOENT);
            }
            return match warp_fk(f.path) {
                WFK_BO_CTL => match self.wbo_ctl(comp, id, a.data) {
                    Ok(()) => p9::build_rwrite(&mut self.out_buf, tag, a.count),
                    Err(e) => self.err(tag, e),
                },
                _ => self.err(tag, p9::E_PERM),
            };
        }
        if !is_surf(f.path) {
            return self.err(tag, p9::E_INVAL);
        }
        let n = surf_n(f.path);
        if !comp.surf_owned(n, self.conn_id, f.gen) {
            return self.err(tag, p9::E_NOENT);
        }
        match surf_fk(f.path) {
            FK_CTL => match self.surface_ctl(comp, n, a.data) {
                Ok(()) => p9::build_rwrite(&mut self.out_buf, tag, a.count),
                Err(e) => self.err(tag, e),
            },
            FK_PRESENT => match self.present(comp, n, a.data) {
                Ok(()) => p9::build_rwrite(&mut self.out_buf, tag, a.count),
                Err(e) => self.err(tag, e),
            },
            _ => self.err(tag, p9::E_PERM),
        }
    }

    /// ctx ctl verbs (Warp-2c): `capset <id>` + `rings <n>` record the
    /// client's declaration at the seam (GPU-DESIGN section 4.3 -- the ABI
    /// carries them from day one; the device sees them when F_CONTEXT_INIT /
    /// per-ring fencing are negotiated); `destroy` retires the ctx.
    fn wctx_ctl(&mut self, comp: &mut Comp, id: u32, data: &[u8]) -> Result<(), u32> {
        let s = core::str::from_utf8(data).map_err(|_| p9::E_INVAL)?;
        let mut it = s.split_ascii_whitespace();
        match it.next() {
            Some("capset") => {
                let v: u32 = it.next().and_then(|t| t.parse().ok()).ok_or(p9::E_INVAL)?;
                comp.wctx_mut(id, self.conn_id).ok_or(p9::E_NOENT)?.capset = v;
                Ok(())
            }
            Some("rings") => {
                let v: u32 = it.next().and_then(|t| t.parse().ok()).ok_or(p9::E_INVAL)?;
                if !(1..=64).contains(&v) {
                    return Err(p9::E_INVAL);
                }
                comp.wctx_mut(id, self.conn_id).ok_or(p9::E_NOENT)?.rings = v;
                Ok(())
            }
            Some("destroy") => {
                let slot = comp.wctx_slot(id).ok_or(p9::E_NOENT)?;
                if comp.wctx(id, self.conn_id).is_none() {
                    return Err(p9::E_NOENT);
                }
                comp.wctx_retire(slot);
                Ok(())
            }
            // #240 (GPU-DESIGN 4.5.4b): run one health probe NOW and fold
            // the result into `stream-rejected`. The verb exists so CADENCE
            // is the client's -- per frame, every Nth, or never -- because
            // the server cannot know how much a given client is willing to
            // pay for the answer. Deliberately returns Ok on every outcome
            // of a probe that RAN: the ANSWER lives in the ctl fields, and
            // an unknown (a transport failure mid-probe) is not a failure
            // of the WRITE.
            // Erroring here would tempt a client to treat a probe it could
            // not run as a rejection, which is the direction that
            // manufactures false deaths.
            //
            // Audit F7: refused while this ctx has fenced work outstanding.
            // The probe rides the SYNCHRONOUS `.step` slot on the client's
            // own ctx, so it queues behind that client's GL work; past
            // SUBMIT_DEADLINE_MS the engine latches `dead`, which is
            // terminal for the whole compositor -- the process that IS the
            // console. A client could reach that with nothing but a ctx and
            // a deep queue. E_AGAIN (not E_IO) is the honest code: the ctx
            // may be perfectly healthy, the question is merely unanswerable
            // right now, and an ERROR on the write is what keeps that
            // distinct from "asked and healthy" without the client having
            // to infer it from an unmoved counter.
            //
            // ROUND-2 F1: `fences_in_flight` ALONE admitted the
            // maximum-hazard state. `reap_abandoned` takes the fenced slot
            // and pushes an `abandoned` tag, which decrements this counter
            // to 0 -- and `gpu.ctx_fences_in_flight` counts the same
            // now-empty slot, so NEITHER gauge can see it. But "abandoned"
            // means precisely "the GL work has not finished in 30 s", i.e.
            // the one state where the readback is most certain to block
            // past the 500 ms deadline. `fence_poisoned` is the flag that
            // records it, and `warp_fenced_admit` already refuses on it one
            // lane over -- the synchronous lane was gated more weakly than
            // the fenced lane sitting beside it.
            Some("verify") => {
                let c = comp.wctx(id, self.conn_id).ok_or(p9::E_NOENT)?;
                if c.fences_in_flight > 0 || c.fence_poisoned {
                    return Err(p9::E_AGAIN);
                }
                comp.warp_ctx_verify(id, self.conn_id);
                Ok(())
            }
            Some("present-to") => {
                // Warp-4, the adoption's CTX half: consent to displaying
                // this ctx's BO <bo_pub> on surface <n>. The gen of the
                // surface incarnation is captured HERE, so a later tenant
                // of the same slot can never inherit the consent. Pure
                // naming -- geometry (and both sides' liveness) gate
                // ACTIVITY per-use in `gl_adoption`, never this write, so
                // a resize racing the handshake degrades to inactive
                // instead of failing the verb.
                let a1 = it.next().ok_or(p9::E_INVAL)?;
                if a1 == "off" {
                    let c = comp.wctx_mut(id, self.conn_id).ok_or(p9::E_NOENT)?;
                    let old = c.present_to.take();
                    if let Some((sl, _, _)) = old {
                        // Only perturb the surface if it actually names THIS
                        // ctx (Warp-5 F1): present-to's surface lives on
                        // another conn, so an ungated retarget + res_stale
                        // let an unprivileged ctx soft-Off a stranger's
                        // direct scanout at will. A surface naming a
                        // different ctx (or none) was never fed by us --
                        // leave it; its owner's glsrc drives its own switch.
                        if comp.surf(sl).map_or(false, |s| s.gl_src == Some(id)) {
                            if let Some(s) = comp.surf_mut(sl) {
                                s.res_stale = true;
                            }
                            comp.gl_retarget(sl);
                        }
                    }
                    return Ok(());
                }
                let sn: usize = a1.parse().map_err(|_| p9::E_INVAL)?;
                let bo: u32 = it.next().and_then(|t| t.parse().ok()).ok_or(p9::E_INVAL)?;
                if it.next().is_some() {
                    return Err(p9::E_INVAL);
                }
                {
                    let c = comp.wctx(id, self.conn_id).ok_or(p9::E_NOENT)?;
                    if !c
                        .bos
                        .iter()
                        .flatten()
                        .any(|b| b.pub_id == bo && !b.retiring && b.dma_fd >= 0)
                    {
                        return Err(p9::E_NOENT);
                    }
                }
                let gen = match comp.surf(sn) {
                    Some(s) => s.gen,
                    None => return Err(p9::E_NOENT),
                };
                if let Some(c) = comp.wctx_mut(id, self.conn_id) {
                    c.present_to = Some((sn, gen, bo));
                }
                // Re-arm the surface's direct switch only if it names THIS
                // ctx (Warp-5 F1). If the surface has not yet glsrc'd us the
                // pairing is not active anyway, and its own glsrc write will
                // retarget when it lands -- so a foreign surface is never
                // touched by our present-to.
                if comp.surf(sn).map_or(false, |s| s.gl_src == Some(id)) {
                    comp.gl_retarget(sn);
                }
                Ok(())
            }
            _ => Err(p9::E_INVAL),
        }
    }

    /// bo ctl verbs (Warp-2c): `create3d <target> <format> <bind> <w> <h>
    /// <d> <array> <last_level> <samples> <flags> <size>` builds the backing
    /// (kernel GPU-BO mint + 3D resource + ctx attach + backing attach);
    /// `destroy` retires the BO. W2d adds the fenced transfers:
    /// `transfer_to <level> <x> <y> <z> <w> <h> <d> <offset> <stride>
    /// <layer_stride>` and `transfer_from ...` (device <-> backing; the
    /// completion rides the owning ctx's fence file). The parameter tuple
    /// is the client's -- the host renderer owns 3D validity (section
    /// 2.1); the server owns only the backing-size sanity its own
    /// soundness needs.
    fn wbo_ctl(&mut self, comp: &mut Comp, id: u32, data: &[u8]) -> Result<(), u32> {
        let s = core::str::from_utf8(data).map_err(|_| p9::E_INVAL)?;
        let mut it = s.split_ascii_whitespace();
        match it.next() {
            Some("create3d") => {
                // #218 server half: a create3d that did not succeed CONSUMES
                // its mint record. The stock client treats a refused create3d
                // as terminal for the BO -- it neither retries nor destroys
                // it -- so the minted-but-unbuilt corpse used to sit in
                // `bos[]` for the ctx's life, and ~1000 per-texture refusals
                // starved `wbo_mint` into total BO death (the #198 cascade's
                // second stage). Hooked HERE, not per-arm in `wbo_create`, so
                // the parse/OPNOTSUPP arms that never reach it consume the
                // mint too; the helper's own guards make the benign
                // already-built refusal (and a foreign conn's) a no-op.
                let ctx_pub = comp.wbo(id, self.conn_id).map(|(c, _)| c.pub_id);
                let r = self.wbo_ctl_create3d(comp, id, it);
                if let Err(e) = r {
                    comp.wbo_unmint_refused(id, self.conn_id);
                    // The #198-hunt accounting: EVERY refusal family passes
                    // this chokepoint. A refusal whose record resolved
                    // counts on ITS ctx; one whose record did not (E_NOENT
                    // always; E_INVAL/E_OPNOTSUPP on a dead id) counts on
                    // the comp-level twin -- per-ctx alone would make a
                    // retry storm against consumed records census-invisible
                    // (audit F3). The families that never reach wbo_create
                    // name themselves below (E_IO already named its arm in
                    // there; naming it again would double-report).
                    let mut counted = false;
                    if let Some(cp) = ctx_pub {
                        if let Some(c) = comp.wctx_mut(cp, self.conn_id) {
                            c.create_refused += 1;
                            counted = true;
                        }
                    }
                    if !counted {
                        comp.warp_create_refused_noctx += 1;
                    }
                    let cp = ctx_pub.unwrap_or(0);
                    if e == p9::E_INVAL {
                        comp.wbo_diag_once(cp, self.conn_id, Comp::WDIAG_CTL_PARSE, "ctl-parse", data.len() as i64, 0, 0, 0, 0, 0);
                    } else if e == p9::E_NOENT {
                        comp.wbo_diag_once(cp, self.conn_id, Comp::WDIAG_CTL_NO_RECORD, "ctl-no-record", id as i64, 0, 0, 0, 0, 0);
                    } else if e == p9::E_OPNOTSUPP {
                        comp.wbo_diag_once(cp, self.conn_id, Comp::WDIAG_CTL_NOT_VIRGL, "ctl-not-virgl", 0, 0, 0, 0, 0, 0);
                    }
                }
                r
            }
            Some("transfer_to") => self.wbo_transfer(comp, id, true, it),
            Some("transfer_from") => self.wbo_transfer(comp, id, false, it),
            Some("destroy") => {
                if comp.wbo_destroy(id, self.conn_id) {
                    Ok(())
                } else {
                    Err(p9::E_NOENT)
                }
            }
            _ => Err(p9::E_INVAL),
        }
    }

    /// The create3d verb body, extracted so the ctl arm can unmint on ANY
    /// failure (#218) -- including the arms that fail before `wbo_create`.
    fn wbo_ctl_create3d(
        &mut self,
        comp: &mut Comp,
        id: u32,
        mut it: core::str::SplitAsciiWhitespace<'_>,
    ) -> Result<(), u32> {
        if !comp.gpu.virgl {
            return Err(p9::E_OPNOTSUPP);
        }
        let mut arg = |_name: &str| -> Result<u64, u32> {
            it.next().and_then(|t| t.parse().ok()).ok_or(p9::E_INVAL)
        };
        let target = arg("target")? as u32;
        let format = arg("format")? as u32;
        let bind = arg("bind")? as u32;
        let w = arg("w")? as u32;
        let h = arg("h")? as u32;
        let d = arg("d")? as u32;
        let array = arg("array")? as u32;
        let last_level = arg("last_level")? as u32;
        let samples = arg("samples")? as u32;
        let flags = arg("flags")? as u32;
        let size = arg("size")?;
        let ctx_pub = comp
            .wbo(id, self.conn_id)
            .map(|(c, _)| c.pub_id)
            .ok_or(p9::E_NOENT)?;
        if comp.wbo_create(
            ctx_pub, id, self.conn_id, target, format, bind, w, h, d, array,
            last_level, samples, flags, size,
        ) {
            Ok(())
        } else {
            Err(E_IO)
        }
    }

    /// The W2d transfer verb tail: `<level> <x> <y> <z> <w> <h> <d>
    /// <offset> <stride> <layer_stride>` -> the fenced TRANSFER chain.
    fn wbo_transfer(
        &mut self,
        comp: &mut Comp,
        id: u32,
        to_host: bool,
        mut it: core::str::SplitAsciiWhitespace<'_>,
    ) -> Result<(), u32> {
        if !comp.gpu.virgl {
            return Err(p9::E_OPNOTSUPP);
        }
        let mut arg = |_name: &str| -> Result<u64, u32> {
            it.next().and_then(|t| t.parse().ok()).ok_or(p9::E_INVAL)
        };
        let level = arg("level")? as u32;
        let x = arg("x")? as u32;
        let y = arg("y")? as u32;
        let z = arg("z")? as u32;
        let w = arg("w")? as u32;
        let h = arg("h")? as u32;
        let d = arg("d")? as u32;
        let offset = arg("offset")?;
        let stride = arg("stride")? as u32;
        let layer_stride = arg("layer_stride")? as u32;
        comp.warp_transfer(
            id, self.conn_id, to_host, level, x, y, z, w, h, d, offset, stride, layer_stride,
        )
        .map(|_| ())
    }

    /// cfg-3 (AURORA-CONFIG.md section 3.3): does this conn's LIVE peer
    /// hold the console-RENDERER role? t_srv_peer resolves the identity
    /// fresh under the proc-table lock, so a moved/died role revokes on
    /// the next authority write. Fail-closed on any error.
    fn peer_is_renderer(&self) -> bool {
        let mut info = TSrvPeerInfo::default();
        if unsafe { t_srv_peer(self.handle, &mut info) } != 0 {
            return false;
        }
        info.alive == 1 && (info.flags & T_SRV_PEER_FLAG_CONSOLE_RENDERER) != 0
    }

    /// The ONLY intentionally-ungated global-ctl verbs (SA-1): the section
    /// 18.6 determinism surface, which a NON-renderer (the in-guest
    /// battery) must drive in test builds and which is #880-feature-
    /// stripped to E_OPNOTSUPP in production. The gate is a DENYLIST of
    /// exactly this set, not an allowlist of the known authority prefixes
    /// -- so every FUTURE global verb (gaps/chord/a typo) is gated BY
    /// CONSTRUCTION, realizing the scripture's "a new global verb defaults
    /// to GATED" (an allowlist would silently ungate a verb added without
    /// touching the gate line).
    fn is_ungated_ctl(s: &str) -> bool {
        s == "test-mode on"
            || s == "test-mode off"
            || s == "tick"
            || s.starts_with("release")
    }

    fn global_ctl(&mut self, comp: &mut Comp, data: &[u8]) -> Result<(), u32> {
        let s = core::str::from_utf8(data).map_err(|_| p9::E_INVAL)?;
        let s = s.trim();
        // The apply-authority gate (cfg-3; the ARCH section 25.4 cfg-3
        // addendum is the prosecution list): every AUTHORITY-BEARING
        // global verb -- mode, clock-rate, and every future global
        // mutation -- admits only a conn whose LIVE peer holds the
        // console-renderer role. Checked per write (revocation-correct).
        // Default-DENY: only the determinism verbs are exempt (see
        // is_ungated_ctl); ctl READS stay ungated (the geometry query,
        // a separate read path).
        if !Self::is_ungated_ctl(s) && !self.peer_is_renderer() {
            return Err(p9::E_PERM);
        }
        if s == "mode auto" {
            // Re-probe the host's preferred rect and adopt it (base
            // virtio-gpu reports one rect, not a mode list). Absent or
            // probe-failed: fail soft, current mode stands.
            let probed = comp.gpu.query_display_info().ok().flatten();
            return match probed {
                Some((w, h)) => comp.set_mode(w, h),
                None => Err(p9::E_AGAIN),
            };
        }
        if let Some(rest) = s.strip_prefix("mode ") {
            let mut it = rest.split_ascii_whitespace();
            let w: u32 = it.next().ok_or(p9::E_INVAL)?.parse().map_err(|_| p9::E_INVAL)?;
            let h: u32 = it.next().ok_or(p9::E_INVAL)?.parse().map_err(|_| p9::E_INVAL)?;
            if it.next().is_some() {
                return Err(p9::E_INVAL);
            }
            return comp.set_mode(w, h);
        }
        if let Some(rate) = s.strip_prefix("clock-rate ") {
            let hz: u32 = rate.trim().parse().map_err(|_| p9::E_INVAL)?;
            if !(1..=240).contains(&hz) {
                return Err(p9::E_INVAL);
            }
            comp.clock_hz = hz;
            return Ok(());
        }
        // cfg-4: the runtime chord table + gaps (AURORA-CONFIG.md section
        // 3.5). Authority verbs (default-deny gated above). A rebind
        // mutates only comp.chords -- NEVER the chord_down swallow-set --
        // so a live remap can never leak a half key-pair (the cfg-4
        // obligation). `chord-reset` (the environment's reset-first push)
        // must precede the strip on "chord " so it is not mis-parsed.
        if s == "chord-reset" {
            comp.chords.reset();
            return Ok(());
        }
        if let Some(rest) = s.strip_prefix("chord ") {
            let mut it = rest.split_ascii_whitespace();
            let combo = it.next().ok_or(p9::E_INVAL)?;
            let action = it.next().ok_or(p9::E_INVAL)?;
            if it.next().is_some() {
                return Err(p9::E_INVAL);
            }
            return comp.chords.bind(combo, action).map_err(|_| p9::E_INVAL);
        }
        if let Some(rest) = s.strip_prefix("gaps ") {
            let px: u32 = rest.trim().parse().map_err(|_| p9::E_INVAL)?;
            comp.chords.set_gaps(px).map_err(|_| p9::E_INVAL)?;
            // The inset feeds recompute; re-run it so the change is visible
            // without waiting for the next layout mutation.
            comp.reconcile();
            return Ok(());
        }
        // Section 18.6 determinism mode (G-6c) -- compiled only into
        // dev/test builds (the `test-mode` cargo feature; the #880
        // strip-for-production class). `test-mode on` freezes the FRAME
        // clock (the serve loop stops wall-clock ticks; queued FRAME
        // events drain normally -- the F15 transition discipline for a
        // synchronous single-threaded engine); `tick` then drives time
        // one step per write; `release [<surface>]` flushes held
        // presents (F13; ownership-gated -- only the caller's surfaces).
        #[cfg(feature = "test-mode")]
        {
            if s == "test-mode on" {
                comp.test_mode = true;
                return Ok(());
            }
            if s == "test-mode off" {
                // No stuck regions: leaving the mode flushes every hold.
                for n in 0..MAX_SURFACES {
                    comp.release_held(n);
                }
                comp.test_mode = false;
                return Ok(());
            }
            if s == "tick" {
                if !comp.test_mode {
                    return Err(p9::E_INVAL); // the wall clock owns time
                }
                comp.frame_tick();
                return Ok(());
            }
            // #178: the warp poisoned-path levers used to ALSO live here,
            // and #175 called them "unreachable" once they moved to the
            // warp W_CTL. That was true only from the PROVER's vantage
            // point -- a warp client has no path to this tree, but every
            // TAPESTRY client does, and these arms drove the GLOBAL
            // abandon/hold. A second door to the same box-wide wedge,
            // opened by the commit that claimed to close it. They exist
            // in exactly one place now: the W_CTL arm, scoped to the
            // caller's own ctx -- which a tapestry client does not have.
            if let Some(rest) = s.strip_prefix("release") {
                let rest = rest.trim();
                if rest.is_empty() {
                    for n in 0..MAX_SURFACES {
                        if comp.surf(n).map_or(false, |s| s.owner_conn == self.conn_id) {
                            comp.release_held(n);
                        }
                    }
                    return Ok(());
                }
                let n: usize = rest.parse().map_err(|_| p9::E_INVAL)?;
                if !comp.surf(n).map_or(false, |s| s.owner_conn == self.conn_id) {
                    return Err(p9::E_BADF); // F2: release only your own
                }
                comp.release_held(n);
                return Ok(());
            }
        }
        #[cfg(not(feature = "test-mode"))]
        if s.starts_with("test-mode")
            || s == "tick"
            || s.starts_with("release")
        {
            return Err(p9::E_OPNOTSUPP); // stripped for production (#880)
        }
        Err(p9::E_INVAL)
    }

    fn surface_ctl(&mut self, comp: &mut Comp, n: usize, data: &[u8]) -> Result<(), u32> {
        let s = core::str::from_utf8(data).map_err(|_| p9::E_INVAL)?;
        let s = s.trim();
        if let Some(rest) = s.strip_prefix("create ") {
            let mut it = rest.split_ascii_whitespace();
            let w: u32 = it.next().ok_or(p9::E_INVAL)?.parse().map_err(|_| p9::E_INVAL)?;
            let h: u32 = it.next().ok_or(p9::E_INVAL)?.parse().map_err(|_| p9::E_INVAL)?;
            if it.next().is_some() {
                return Err(p9::E_INVAL);
            }
            return comp.create(n, w, h);
        }
        if s == "destroy" {
            comp.retire(n);
            return Ok(());
        }
        if let Some(t) = s.strip_prefix("title ") {
            if let Some(surf) = comp.surf_mut(n) {
                surf.title = String::from(t.trim());
            }
            return Ok(());
        }
        if let Some(rest) = s.strip_prefix("resize ") {
            // The section-18.3 resize ack: `resize W H <serial>` echoes a
            // CONFIGURE offer; a successful Rwrite IS the generation
            // fence (see resize_ack).
            let mut it = rest.split_ascii_whitespace();
            let w: u32 = it.next().ok_or(p9::E_INVAL)?.parse().map_err(|_| p9::E_INVAL)?;
            let h: u32 = it.next().ok_or(p9::E_INVAL)?.parse().map_err(|_| p9::E_INVAL)?;
            let serial: u16 =
                it.next().ok_or(p9::E_INVAL)?.parse().map_err(|_| p9::E_INVAL)?;
            if it.next().is_some() {
                return Err(p9::E_INVAL);
            }
            return comp.resize_ack(n, w, h, serial);
        }
        if let Some(rest) = s.strip_prefix("glsrc ") {
            // Warp-4, the adoption's SURFACE half: accept warp ctx
            // <pub_id> as this surface's display source. Naming a ctx
            // grants NOTHING by itself -- display activates only when
            // that ctx's own `present-to` names this surface incarnation
            // back (mutual adoption), so no ownership check is needed
            // here: a surface naming a stranger's ctx just never
            // activates. The ctx must exist NOW (a mis-sequenced
            // handshake fails loud, the #178 precedent); liveness is
            // re-checked at every use.
            let rest = rest.trim();
            if rest == "off" {
                if let Some(surf) = comp.surf_mut(n) {
                    surf.gl_src = None;
                    // The weave never saw the GL frames; the 2D machinery
                    // heals from stale on its next switch.
                    surf.res_stale = true;
                }
                comp.gl_retarget(n);
                return Ok(());
            }
            let v: u32 = rest.parse().map_err(|_| p9::E_INVAL)?;
            if !comp
                .warp_ctxs
                .iter()
                .flatten()
                .any(|c| c.pub_id == v && !c.retiring)
            {
                return Err(p9::E_NOENT);
            }
            if let Some(surf) = comp.surf_mut(n) {
                surf.gl_src = Some(v);
            }
            comp.gl_retarget(n);
            return Ok(());
        }
        Err(p9::E_INVAL)
    }

    /// The present engine (section 18.2): parse + validate the tpresent
    /// descriptor against the surface geometry (the untrusted-client
    /// boundary), then TRANSFER + FLUSH synchronously. The Rwrite this
    /// returns becomes the client's CQE -- the D1 recycle gate.
    ///
    /// Multi-rect (G-6c): rect_count k >= 2 carries rects 1..k inline
    /// after the header (payload 32 + 16*(k-1); count bounded; EVERY rect
    /// validated before any pixel work -- no partial present). HOLD
    /// (G-6c, test-mode only): the pixel work runs normally INSIDE this
    /// dispatch (tearing-freedom intact) but the device-visible flush
    /// defers to `release`; a later non-HOLD present flushes it
    /// implicitly.
    fn present(&mut self, comp: &mut Comp, n: usize, data: &[u8]) -> Result<(), u32> {
        if data.len() < TPRESENT_LEN {
            return Err(p9::E_INVAL);
        }
        let word = |o: usize| u32::from_le_bytes([data[o], data[o + 1], data[o + 2], data[o + 3]]);
        let version = word(0);
        let slot = word(4);
        let flags = word(8);
        let rect_count = word(12);

        if version != TPRESENT_V1 {
            return Err(p9::E_INVAL);
        }
        let hold = flags & TPRESENT_HOLD != 0;
        #[cfg(feature = "test-mode")]
        if hold && !comp.test_mode {
            return Err(p9::E_OPNOTSUPP); // section 18.6: determinism-mode only
        }
        #[cfg(not(feature = "test-mode"))]
        if hold {
            return Err(p9::E_OPNOTSUPP); // stripped for production (#880)
        }
        if rect_count > TPRESENT_MAX_RECTS {
            return Err(p9::E_INVAL);
        }
        let expect = if rect_count <= 1 {
            TPRESENT_LEN
        } else {
            TPRESENT_LEN + (rect_count as usize - 1) * TRECT_LEN
        };
        if data.len() != expect {
            return Err(p9::E_INVAL);
        }

        let (w, h, slot_stride, res, state) = {
            let s = comp.surf(n).ok_or(p9::E_BADF)?;
            (s.w, s.h, s.slot_stride, s.resource_id, s.state)
        };
        if state == SurfState::Minted {
            return Err(p9::E_INVAL); // no weave yet
        }
        if slot >= WEAVE_SLOTS {
            return Err(p9::E_INVAL);
        }
        // Collect + validate EVERY rect up front (overflow-safe: u32 +
        // u32 in u64). rect_count 0 = full-surface damage.
        let mut rects: Vec<(u32, u32, u32, u32)> = Vec::new();
        if rect_count == 0 {
            rects.push((0, 0, w, h));
        } else {
            for i in 0..rect_count as usize {
                let o = if i == 0 { 16 } else { TPRESENT_LEN + (i - 1) * TRECT_LEN };
                rects.push((word(o), word(o + 4), word(o + 8), word(o + 12)));
            }
        }
        for &(x, y, pw, ph) in &rects {
            if pw == 0
                || ph == 0
                || (x as u64) + (pw as u64) > w as u64
                || (y as u64) + (ph as u64) > h as u64
            {
                return Err(p9::E_INVAL);
            }
        }

        // #164: past every validation gate = a well-formed present, on
        // any routing arm below. Malformed spam never reaches this line,
        // so it cannot hold the clock awake; hidden-surface presents are
        // filtered inside (audit F1).
        comp.note_present(n);

        // The #56 present-style latch: a present whose damage does not
        // cover the full surface marks the client an ACCUMULATOR --
        // placement (blit + ptr_hit) then crops instead of letterboxing.
        // Checked on EVERY present (incl. direct-scanout mode, where
        // placement is moot but the latch must stay accurate for a later
        // return to composed mode). The cover test is the EXACT union
        // (rects_cover_full): the battery's multi-rect leg presents the
        // full frame as two tiles, which a single-full-rect shortcut
        // falsely latched (the moveB pane-center regression).
        if !rects_cover_full(&rects, w, h) {
            if let Some(s) = comp.surf_mut(n) {
                s.patchwork = true;
            }
        }

        // Route by scanout mode (G-6). The slot base + rect origin ride
        // the TRANSFER offset; rows advance by the resource stride (w*4).
        if comp.pending_direct == Some(n) {
            if hold {
                // A held present must not complete a scanout SWITCH (the
                // switch IS composition); present unheld once first.
                return Err(E_AGAIN);
            }
            // Warp-4: an ACTIVE GL adoption switches to the client's own
            // 3D resource -- the frame is already host-side, so there is
            // no guest transfer at all; bind then flush (#57: a flush
            // before the bind is dropped by spec). The WEAVE stays stale
            // by construction (GL frames never land in it), so res_stale
            // is forced TRUE -- the 2D machinery heals from it whenever
            // the adoption later ends.
            if let Some(g) = comp.gl_adoption(n) {
                say!("tapestryd: scanout direct {} GL res {} ({}x{})", n, g.res_id, w, h);
                if comp.gpu.set_scanout(g.res_id, w, h).is_ok() {
                    comp.bound_res = g.res_id;
                    let _ = comp.gpu.flush(g.res_id, 0, 0, w, h);
                    comp.scanout = Scanout::Direct(n);
                    comp.pending_direct = None;
                }
                if let Some(s) = comp.surf_mut(n) {
                    s.res_stale = true;
                }
            } else {
                // The deferred direct switch (F16: SET_SCANOUT only at a
                // present-COMPLETE). A stale client resource (composed-era
                // presents never transferred to it) expands this transfer
                // to the full surface first.
                let stale = comp.surf(n).map_or(false, |s| s.res_stale);
                let xfer: Vec<(u32, u32, u32, u32)> =
                    if stale { alloc::vec![(0, 0, w, h)] } else { rects.clone() };
                for &(tx, ty, tw, th) in &xfer {
                    let offset = (slot as u64) * slot_stride
                        + ((ty as u64) * (w as u64) + tx as u64) * 4;
                    if comp.gpu.transfer(res, offset, tx, ty, tw, th).is_err() {
                        return Err(E_IO);
                    }
                    if comp.gpu.flush(res, tx, ty, tw, th).is_err() {
                        return Err(E_IO);
                    }
                }
                say!("tapestryd: scanout direct {} ({}x{})", n, w, h);
                if comp.gpu.set_scanout(res, w, h).is_ok() {
                    // Post-bind full flush (#57): the per-rect flushes
                    // above targeted a not-yet-scanned-out resource
                    // (dropped by spec), and cocoa's same-size surface
                    // replace renders nothing -- without this the display
                    // keeps the stale composed frame until later client
                    // damage covers it (the lingering-dead-pane symptom).
                    let _ = comp.gpu.flush(res, 0, 0, w, h);
                    comp.bound_res = res;
                    comp.scanout = Scanout::Direct(n);
                    comp.pending_direct = None;
                    if let Some(s) = comp.surf_mut(n) {
                        s.res_stale = false;
                    }
                }
            }
        } else if comp.scanout == Scanout::Direct(n) {
            if let Some(g) = comp.gl_adoption(n) {
                // Warp-4 steady-state GL direct: the render already
                // happened host-side (the client's SUBMIT_3D was queued
                // before this present arrived -- same controlq, FIFO), so
                // a present is ONLY the display flush. Damage rects are
                // ignored: GL frames are whole-frame by nature (the SDL
                // swap presents full damage), and a partial flush of a 3D
                // scanout buys nothing. HOLD is refused: its contract is
                // a DEFERRED device-visible flush, and the GL arms have
                // no deferral -- silently flushing now would make the
                // determinism battery's hold legs lie.
                if hold {
                    return Err(p9::E_OPNOTSUPP);
                }
                if comp.bound_res != g.res_id {
                    // Defensive (mode-machine-unreachable: every adoption
                    // change routes through the pending switch): rebind
                    // rather than flush a non-scanned-out resource, which
                    // the spec drops silently.
                    say!("tapestryd: GL direct rebind {} -> {}", comp.bound_res, g.res_id);
                    if comp.gpu.set_scanout(g.res_id, w, h).is_err() {
                        return Err(E_IO);
                    }
                    comp.bound_res = g.res_id;
                }
                if comp.gpu.flush(g.res_id, 0, 0, w, h).is_err() {
                    return Err(E_IO);
                }
                if let Some(s) = comp.surf_mut(n) {
                    s.res_stale = true;
                }
                if !hold {
                    comp.release_held(n);
                }
                // The completion tail, mirrored from the end of this
                // function (presents count + Woven->Live + the displaced-
                // generation retire) -- keep in lockstep with it. The
                // retire is correct here for the same reason as there:
                // the display shows CURRENT content (the adopted BO), so
                // no scanout references the displaced weave's resource.
                {
                    let s = comp.surf_mut(n).unwrap();
                    s.presents += 1;
                    if s.state == SurfState::Woven {
                        s.state = SurfState::Live;
                    }
                }
                if let Some((oldw, old_res)) =
                    comp.surf_mut(n).and_then(|s| s.old_weave.take())
                {
                    comp.release_gen(&oldw, old_res);
                }
                return Ok(());
            }
            // The stage-0 direct path, byte-identical for the single-rect
            // form: damage transfer + flush on the client's own
            // scanned-out resource (the zero-copy fullscreen case). A
            // held present transfers but defers every flush to release.
            let mut acc: Option<Rect> = None;
            for &(x, y, pw, ph) in &rects {
                let offset =
                    (slot as u64) * slot_stride + ((y as u64) * (w as u64) + x as u64) * 4;
                if comp.gpu.transfer(res, offset, x, y, pw, ph).is_err() {
                    return Err(E_IO);
                }
                if hold {
                    acc = Some(rect_union(
                        acc.unwrap_or(Rect::ZERO),
                        Rect { x, y, w: pw, h: ph },
                    ));
                } else if comp.gpu.flush(res, x, y, pw, ph).is_err() {
                    return Err(E_IO);
                }
            }
            if let Some(r) = acc {
                let held = match comp.surf(n).and_then(|s| s.held) {
                    Some(Held::Direct(prev)) => Held::Direct(rect_union(prev, r)),
                    _ => Held::Direct(r), // a stale Composed hold is superseded
                };
                if let Some(s) = comp.surf_mut(n) {
                    s.held = Some(held);
                }
            } else if !hold {
                // A non-HOLD present flushes any held region implicitly
                // (F13: no stuck regions; the union already includes the
                // most-recent bytes).
                comp.release_held(n);
            }
        } else if comp.scanout == Scanout::Composed {
            // Warp-4 composed GL (the ladder's readback fallback): pull
            // the adopted frame host->guest into the BO's own backing --
            // synchronously, so the present stays one dispatch (the I-40
            // premise) -- then compose those pages like any weave.
            // Full-frame always: GL damage is whole-frame by nature, and
            // the transfer cost dwarfs any rect bookkeeping.
            if let Some(g) = comp.gl_adoption(n) {
                if hold {
                    return Err(p9::E_OPNOTSUPP); // no GL deferral (see the direct arm)
                }
                if comp
                    .gpu
                    .transfer_from_3d_sync(g.dev_ctx, g.res_id, g.w, g.h, g.w * 4)
                    .is_ok()
                {
                    if let Some(r) =
                        comp.blit_composed_pixels(n, 0, 0, 0, w, h, Some(g.va))
                    {
                        comp.screen_push(r);
                    }
                }
                if let Some(s) = comp.surf_mut(n) {
                    s.res_stale = true;
                }
                if !hold {
                    comp.release_held(n);
                }
                // The completion tail, mirrored (see the GL direct arm).
                {
                    let s = comp.surf_mut(n).unwrap();
                    s.presents += 1;
                    if s.state == SurfState::Woven {
                        s.state = SurfState::Live;
                    }
                }
                if let Some((oldw, old_res)) =
                    comp.surf_mut(n).and_then(|s| s.old_weave.take())
                {
                    comp.release_gen(&oldw, old_res);
                }
                return Ok(());
            }
            // Composed: blit the damage into the screen buffer at the
            // pane's content rect (a hidden/unhosted surface skips); the
            // client resource is now stale for a future direct switch.
            // Held presents blit NOW (weave bytes read only inside this
            // dispatch) and defer only the screen push.
            let mut acc: Option<Rect> = None;
            for &(x, y, pw, ph) in &rects {
                if let Some(r) = comp.blit_composed_pixels(n, slot, x, y, pw, ph, None) {
                    if hold {
                        acc = Some(rect_union(acc.unwrap_or(Rect::ZERO), r));
                    } else {
                        comp.screen_push(r);
                    }
                }
            }
            if let Some(r) = acc {
                let held = match comp.surf(n).and_then(|s| s.held) {
                    Some(Held::Composed(prev)) => Held::Composed(rect_union(prev, r)),
                    _ => Held::Composed(r), // a stale Direct hold is superseded
                };
                if let Some(s) = comp.surf_mut(n) {
                    s.held = Some(held);
                }
            } else if !hold {
                comp.release_held(n);
            }
            if let Some(s) = comp.surf_mut(n) {
                s.res_stale = true;
            }
        } else {
            // Boot / Off / another surface's Direct: the present completes
            // without pixels (D1 contract kept; content heals on later
            // presents once visible).
            if let Some(s) = comp.surf_mut(n) {
                s.res_stale = true;
            }
        }

        {
            let s = comp.surf_mut(n).unwrap();
            s.presents += 1;
            if s.state == SurfState::Woven {
                s.state = SurfState::Live;
            }
        }
        // The first post-fence present retires the displaced generation
        // (the spec's RetireDisplaced + ServerRelease): the display now
        // shows current-generation content -- composed blits COPY (the
        // screen resource references no client weave) and the direct arms
        // target the current resource -- and quiesce holds by construction
        // (presents complete inside one dispatch), so the old weave's
        // server refs drop here. The client's own old mapping drains via
        // its weave-fid clunk (ClunkMap; #847 keeps the pages until then).
        if let Some((oldw, old_res)) = comp.surf_mut(n).and_then(|s| s.old_weave.take()) {
            comp.release_gen(&oldw, old_res);
            // No diagnostic here (#55b): a generation retire is now ROUTINE
            // steady-state traffic -- the fbcon acks every pane resize, so
            // this fires per split/unsplit while the SESSION is printing,
            // and a SYS_PUTS line here interleaves at the UART FIFO with
            // concurrent /dev/cons output (the P1-F carve-out), tearing
            // byte patterns mid-line (the ls-gfx-panes post-battery pwd
            // assert lost `/home/michael` to exactly this print). Aurora's
            // own single `reweave WxH` line carries the diagnostic value.
        }
        Ok(())
    }

    fn h_readdir(&mut self, comp: &mut Comp, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_treaddir(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let i = match self.fid_find(a.fid) {
            Some(i) => i,
            None => return self.err(tag, p9::E_BADF),
        };
        let f = self.fids[i].unwrap();
        if !f.opened || !is_dir(f.path) {
            return self.err(tag, p9::E_NOTDIR);
        }

        // Collect the child list (name, path) for the fid's directory,
        // ownership-filtered (F2: readdir shows only the caller's surfaces).
        let mut names: Vec<(Vec<u8>, u64)> = Vec::new();
        match f.path {
            P_ROOT => {
                names.push((b"ctl".to_vec(), P_CTL));
                names.push((b"surface".to_vec(), P_SURF_DIR));
                names.push((b"layout".to_vec(), P_LAYOUT));
                names.push((b"pane".to_vec(), P_PANE_DIR));
            }
            P_PANE_DIR => {
                for (_slot, id) in comp.layout.live_ids() {
                    let mut nm = String::new();
                    let _ = core::fmt::write(&mut nm, format_args!("{}", id));
                    names.push((nm.into_bytes(), make_pane(id, PFK_DIR)));
                }
            }
            d if is_pane(d) && pane_fk(d) == PFK_DIR => {
                let id = pane_id(d);
                if comp.layout.slot_of_id(id).is_some() {
                    for (nm, fk) in [
                        (&b"ctl"[..], PFK_CTL),
                        (&b"mode"[..], PFK_MODE),
                        (&b"role"[..], PFK_ROLE),
                        (&b"tag"[..], PFK_TAG),
                        (&b"surface"[..], PFK_SURFACE),
                        (&b"geometry"[..], PFK_GEOMETRY),
                    ] {
                        names.push((nm.to_vec(), make_pane(id, fk)));
                    }
                }
            }
            P_SURF_DIR => {
                names.push((b"new".to_vec(), P_SURF_NEW));
                for n in 0..MAX_SURFACES {
                    if comp.surf(n).map_or(false, |s| s.owner_conn == self.conn_id) {
                        let mut nm = String::new();
                        let _ = core::fmt::write(&mut nm, format_args!("{}", n));
                        names.push((nm.into_bytes(), make_surf(n, FK_DIR)));
                    }
                }
            }
            d if is_surf(d) && surf_fk(d) == FK_DIR => {
                let n = surf_n(d);
                if comp.surf_owned(n, self.conn_id, f.gen) {
                    for (nm, fk) in [
                        (&b"ctl"[..], FK_CTL),
                        (&b"weave"[..], FK_WEAVE),
                        (&b"present"[..], FK_PRESENT),
                        (&b"event"[..], FK_EVENT),
                        (&b"geometry"[..], FK_GEOMETRY),
                    ] {
                        names.push((nm.to_vec(), make_surf(n, fk)));
                    }
                }
            }
            // --- The /dev/warp tree (Warp-2c; mirrors walk_child -- a name
            // in one ladder and not the other is walkable-but-invisible). ---
            W_ROOT => {
                names.push((b"ctl".to_vec(), W_CTL));
                names.push((b"caps".to_vec(), W_CAPS));
                names.push((b"ctx".to_vec(), W_CTX_DIR));
            }
            W_CTX_DIR => {
                names.push((b"new".to_vec(), W_CTX_NEW));
                for c in comp.warp_ctxs.iter().flatten() {
                    // `!retiring` matches walk_child (round-3 F4): a name
                    // listed here but rejected there is the inverse of the
                    // walkable-but-invisible trap, and just as wrong.
                    if c.owner_conn == self.conn_id && !c.retiring {
                        let mut nm = String::new();
                        let _ = core::fmt::write(&mut nm, format_args!("{}", c.pub_id));
                        names.push((nm.into_bytes(), make_wctx(c.pub_id, WFK_DIR)));
                    }
                }
            }
            d if is_wctx(d) && warp_fk(d) == WFK_DIR => {
                let id = warp_id(d);
                if comp.wctx(id, self.conn_id).is_some() {
                    for (nm, fk) in [
                        (&b"ctl"[..], WFK_CTL),
                        (&b"submit"[..], WFK_SUBMIT),
                        (&b"fence"[..], WFK_FENCE),
                        (&b"bo"[..], WFK_BO_DIR),
                    ] {
                        names.push((nm.to_vec(), make_wctx(id, fk)));
                    }
                }
            }
            d if is_wctx(d) && warp_fk(d) == WFK_BO_DIR => {
                let cid = warp_id(d);
                if let Some(c) = comp.wctx(cid, self.conn_id) {
                    names.push((b"new".to_vec(), make_wctx(cid, WFK_BO_NEW)));
                    for b in c.bos.iter().flatten().filter(|b| !b.retiring) {
                        let mut nm = String::new();
                        let _ = core::fmt::write(&mut nm, format_args!("{}", b.pub_id));
                        names.push((nm.into_bytes(), make_wbo(b.pub_id, WFK_DIR)));
                    }
                }
            }
            d if is_wbo(d) && warp_fk(d) == WFK_DIR => {
                let bid = warp_id(d);
                if comp.wbo(bid, self.conn_id).is_some() {
                    for (nm, fk) in [
                        (&b"ctl"[..], WFK_BO_CTL),
                        (&b"map"[..], WFK_BO_MAP),
                        (&b"info"[..], WFK_BO_INFO),
                    ] {
                        names.push((nm.to_vec(), make_wbo(bid, fk)));
                    }
                }
            }
            _ => {}
        }

        // The ordinal-cookie pack (the ptyfs shape): entries [offset..)
        // fit within both the request count and the reply frame budget.
        let budget = (self.msize as usize)
            .saturating_sub(p9::P9_HDR_LEN + 4)
            .min(a.count as usize);
        let mut data: Vec<u8> = Vec::new();
        let mut ord: u64 = 0;
        for (nm, path) in &names {
            ord += 1;
            if ord <= a.offset {
                continue;
            }
            let need = p9::dirent_len(nm.len());
            if data.len() + need > budget {
                break;
            }
            let q = self.qid_of(*path);
            let dtype = if is_dir(*path) { p9::DT_DIR } else { p9::DT_REG };
            let mut tmp = alloc::vec![0u8; need];
            let n = p9::pack_dirent(&mut tmp, 0, &q, ord, dtype, nm)?;
            data.extend_from_slice(&tmp[..n]);
        }
        p9::build_rreaddir(&mut self.out_buf, tag, &data)
    }

    fn h_getattr(&mut self, comp: &mut Comp, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let fid = match p9::parse_tgetattr(tmsg) {
            Ok(f) => f,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let i = match self.fid_find(fid) {
            Some(i) => i,
            None => return self.err(tag, p9::E_BADF),
        };
        let f = self.fids[i].unwrap();
        let _ = comp;
        let (mode, nlink) = if is_dir(f.path) {
            (DIR_MODE, 2u64)
        } else {
            (FILE_RW, 1u64)
        };
        // The security trio (mode/uid/gid) MUST be filled: the kernel's
        // dev9p per-component X-search reads them; unfilled fails closed.
        let valid = p9::P9_GETATTR_MODE
            | p9::P9_GETATTR_NLINK
            | p9::P9_GETATTR_UID
            | p9::P9_GETATTR_GID
            | P9_GETATTR_SIZE;
        let q = self.qid_of(f.path);
        p9::build_rgetattr(
            &mut self.out_buf,
            tag,
            valid,
            &q,
            mode,
            T_PRINCIPAL_SYSTEM,
            T_GID_SYSTEM,
            nlink,
            0,
        )
    }

    fn h_clunk(&mut self, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_tclunk(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        if self.fid_find(a.fid).is_none() {
            return self.err(tag, p9::E_BADF);
        }
        self.fid_clunk(a.fid);
        p9::build_rclunk(&mut self.out_buf, tag)
    }

    fn h_flush(&mut self, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_tflush(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        // Cancel site 4 (Tflush): the held reply under oldtag dies; per 9P
        // the client reuses oldtag only after this Rflush.
        self.pending_reads.retain(|pr| pr.tag != a.oldtag);
        self.pending_fences.retain(|pf| pf.tag != a.oldtag);
        p9::build_rflush(&mut self.out_buf, tag)
    }

    /// The Tweft handler (the netd h_weft shape): mint-or-echo the weave's
    /// share registration. ring_entries = 0 is the WEAVE kind contract the
    /// kernel's weft_claimed_kind cross-checks (G-2).
    fn h_weft(&mut self, comp: &mut Comp, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let fid = match p9::parse_tweft(tmsg) {
            Ok(f) => f,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let i = match self.fid_find(fid) {
            Some(i) => i,
            None => return self.err(tag, p9::E_BADF),
        };
        let f = self.fids[i].unwrap();
        // Warp-2c: the BO map fid is the second Tweft anchor (the weave
        // shape verbatim: lazy share, ring_entries 0 = the map-only kind
        // the kernel cross-checks).
        if f.opened && is_wbo(f.path) && warp_fk(f.path) == WFK_BO_MAP {
            let id = warp_id(f.path);
            return match comp.wbo_weft_ensure(id, self.conn_id) {
                Some((share_id, size)) => {
                    p9::build_rweft(&mut self.out_buf, tag, share_id, size, 0)
                }
                None => self.err(tag, p9::E_NOMEM),
            };
        }
        if !f.opened || !is_surf(f.path) || surf_fk(f.path) != FK_WEAVE {
            return self.err(tag, p9::E_INVAL);
        }
        let n = surf_n(f.path);
        if !comp.surf_owned(n, self.conn_id, f.gen) {
            return self.err(tag, p9::E_NOENT);
        }
        match comp.weft_ensure(n) {
            Some((share_id, size)) => {
                p9::build_rweft(&mut self.out_buf, tag, share_id, size, 0)
            }
            None => self.err(tag, p9::E_NOMEM),
        }
    }

    // --- deferred delivery (the loop-top pass) -------------------------------

    /// Deliver held event reads whose surfaces have events (or died: EOF).
    /// False = the conn's transport failed (caller closes it).
    pub fn poll_events(&mut self, comp: &mut Comp) -> bool {
        let mut i = 0;
        while i < self.pending_reads.len() {
            let pr = self.pending_reads[i];
            let alive = comp.surf_owned(pr.surf, self.conn_id, pr.gen);
            if !alive {
                // The surface died with this read parked: EOF the stream.
                if !self.deliver_read(pr.tag, &[]) {
                    return false;
                }
                self.pending_reads.remove(i);
                continue;
            }
            match self.drain_events(comp, pr.surf, pr.cap) {
                Some(count) => {
                    let data = self.scratch_events(comp, pr.surf, pr.cap, count);
                    if !self.deliver_read(pr.tag, &data) {
                        return false;
                    }
                    self.pending_reads.remove(i);
                }
                None => i += 1,
            }
        }
        true
    }

    /// Deliver held fence reads whose ctxs have unreported completions
    /// (or died: EOF the stream). False = the conn's transport failed.
    pub fn poll_fences(&mut self, comp: &mut Comp) -> bool {
        let mut i = 0;
        while i < self.pending_fences.len() {
            let pf = self.pending_fences[i];
            let rec = match comp.wctx(pf.ctx_pub, self.conn_id) {
                None => None, // the ctx died with this read parked: EOF
                // A poisoned ctx has a fence that will NEVER signal
                // (abandoned, not completed). Reporting nothing forever
                // stranded the reader with neither record nor EOF
                // (round-2 F7) -- end the stream so the client learns.
                // Unconditional since round-5 F2 -- see the h_read twin.
                Some(c) if c.fence_poisoned => None,
                Some(c) if c.fence_signaled > c.fence_reported => Some(c.fence_signaled),
                Some(_) => {
                    i += 1;
                    continue;
                }
            };
            let mut s = String::new();
            if let Some(v) = rec {
                comp.wctx_mut(pf.ctx_pub, self.conn_id).unwrap().fence_reported = v;
                let _ = core::fmt::write(&mut s, format_args!("{}\n", v));
            }
            if !self.deliver_read(pf.tag, s.as_bytes()) {
                return false;
            }
            self.pending_fences.remove(i);
        }
        true
    }

    fn deliver_read(&mut self, tag: u16, data: &[u8]) -> bool {
        self.out_buf.clear();
        self.out_buf.resize(SRV_MSIZE_USIZE, 0);
        match p9::build_rread(&mut self.out_buf, tag, data) {
            Ok(len) => self.send_all(len),
            Err(_) => false,
        }
    }
}

/// POSIX EIO (the ninep constant set has no E_IO name): a GPU command
/// failure surfaces to the client as a remote I/O error.
const E_IO: u32 = 5;

/// POSIX EAGAIN: the resize-ack "not now" verdicts (stale serial; a
/// prior reweave still draining) -- the client drains events / presents
/// a frame and re-acks.
const E_AGAIN: u32 = 11;

fn parse_dec(name: &[u8]) -> Option<usize> {
    if name.is_empty() || name.len() > 3 {
        return None;
    }
    let mut v = 0usize;
    for &b in name {
        if !b.is_ascii_digit() {
            return None;
        }
        v = v * 10 + (b - b'0') as usize;
    }
    Some(v)
}

/// Pane ids are monotonic u32s (never reused) -- wider than the surface
/// index parser above.
fn parse_u32(name: &[u8]) -> Option<u32> {
    if name.is_empty() || name.len() > 8 {
        return None;
    }
    let mut v = 0u32;
    for &b in name {
        if !b.is_ascii_digit() {
            return None;
        }
        v = v.checked_mul(10)?.checked_add((b - b'0') as u32)?;
    }
    Some(v)
}
