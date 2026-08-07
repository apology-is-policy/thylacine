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
    t_weft_share, t_weft_unshare, t_yield, TSrvPeerInfo, T_GID_SYSTEM, T_PRINCIPAL_SYSTEM,
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
const MAX_FIDS: usize = 32;
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
const MAX_WARP_BOS_PER_CTX: usize = 16;

/// Triple buffering (D1): one weave carries three page-aligned slots.
const WEAVE_SLOTS: u32 = 3;

/// R2-F4: the bounded per-surface event queue. FRAME coalesces; a
/// non-droppable overflow wedges the surface.
const EVENT_QUEUE_CAP: usize = 128;

const PAGE: u64 = 0x1000;

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
    WARP_FLAG | WARP_CTX | ((id as u64 & N_MASK) << 8) | (fk & FK_MASK)
}
fn make_wbo(id: u32, fk: u64) -> u64 {
    WARP_FLAG | WARP_BO | ((id as u64 & N_MASK) << 8) | (fk & FK_MASK)
}
fn warp_id(path: u64) -> u32 {
    ((path >> 8) & N_MASK) as u32
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
    /// GPU resource ids are PER-GENERATION (a reweave mints a fresh one);
    /// pre-incremented, so the first id is SCREEN_RES + 1.
    res_seq: u32,
    /// The container tree (G-6): hosting, geometry, focus.
    layout: Layout,
    screen: Option<Screen>,
    scanout: Scanout,
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
    warp_ctx_seq: u32,
    warp_bo_seq: u32,
}

/// A GPU-seam rendering context (Warp-2c). Owned by one conn; the DEVICE
/// ctx id is its slot + 1 (reused only after the synchronous CTX_DESTROY).
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
    /// this ctx; the newest retired fence id; the newest id the fence
    /// file has reported. signaled > reported = one unread record.
    fences_in_flight: u32,
    fence_signaled: u64,
    fence_reported: u64,
    bos: [Option<WarpBo>; MAX_WARP_BOS_PER_CTX],
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
}

const NO_SURFACE: Option<Surface> = None;

impl Comp {
    pub fn new(gpu: Gpu) -> Comp {
        Comp {
            gpu,
            surfaces: [NO_SURFACE; MAX_SURFACES],
            gen_seq: 0,
            conn_seq: 0,
            res_seq: SCREEN_RES,
            layout: Layout::new(),
            screen: None,
            scanout: Scanout::Boot,
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
            warp_ctx_seq: 0,
            warp_bo_seq: 0,
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
        self.conn_seq += 1;
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
            presents: 0,
        });
        Some(n)
    }

    fn next_res_id(&mut self) -> u32 {
        self.res_seq += 1;
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
                    let _ = self.gpu.set_scanout(sres, dw, dh);
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
    fn blit_composed_pixels(
        &mut self,
        n: usize,
        slot: u32,
        x: u32,
        y: u32,
        pw: u32,
        ph: u32,
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
        let src_base = weave_va + (slot as u64) * slot_stride;
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
                self.scanout = Scanout::Off;
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
        let px = (self.ptr_x as i32 + dx).clamp(0, dw.max(1) - 1) as u32;
        let py = (self.ptr_y as i32 + dy).clamp(0, dh.max(1) - 1) as u32;
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
const WARP_NO_BO: Option<WarpBo> = None;

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
        if c.owner_conn != conn {
            return None;
        }
        Some(c)
    }

    fn wctx_mut(&mut self, pub_id: u32, conn: u64) -> Option<&mut WarpCtx> {
        let i = self.wctx_slot(pub_id)?;
        let c = self.warp_ctxs[i].as_mut().unwrap();
        if c.owner_conn != conn {
            return None;
        }
        Some(c)
    }

    /// Resolve a live BO (and its ctx pub id) the caller owns.
    fn wbo(&self, bo_pub: u32, conn: u64) -> Option<(&WarpCtx, &WarpBo)> {
        for c in self.warp_ctxs.iter().flatten() {
            if c.owner_conn != conn {
                continue;
            }
            for b in c.bos.iter().flatten() {
                if b.pub_id == bo_pub {
                    return Some((c, b));
                }
            }
        }
        None
    }

    /// Mint a context for `conn` (one per client -- the I-45 exposure
    /// bound): allocate the slot, CTX_CREATE on the device (virgl-gated by
    /// the caller), roll the slot back if the device refuses.
    fn wctx_mint(&mut self, conn: u64) -> Option<u32> {
        if self.warp_ctxs.iter().flatten().any(|c| c.owner_conn == conn) {
            return None; // one ctx per client
        }
        let slot = self.warp_ctxs.iter().position(|c| c.is_none())?;
        let dev_ctx = (slot as u32) + 1;
        if self.gpu.ctx_create(dev_ctx, b"warp").is_err() {
            return None;
        }
        self.warp_ctx_seq = self.warp_ctx_seq.wrapping_add(1);
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
            bos: [WARP_NO_BO; MAX_WARP_BOS_PER_CTX],
        });
        Some(pub_id)
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
        });
        Some(pub_id)
    }

    /// The BO backing build (the ctl `create3d` verb): kernel GPU-BO mint
    /// -> map -> RESOURCE_CREATE_3D -> CTX_ATTACH -> ATTACH_BACKING, with
    /// reverse unwind on any failure. Size is the client's declared backing
    /// length; the kernel envelope + the client's own shared-map budget
    /// bound it -- the server checks only page alignment and non-zero.
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
            return false;
        }
        let c = match self.wctx(ctx_pub, conn) {
            Some(c) => c,
            None => return false,
        };
        let dev_ctx = c.dev_ctx;
        if !c.bos.iter().flatten().any(|b| b.pub_id == bo_pub) {
            return false;
        }
        // Already built? create3d is once per BO.
        if c
            .bos
            .iter()
            .flatten()
            .any(|b| b.pub_id == bo_pub && b.dma_fd >= 0)
        {
            return false;
        }

        let fd =
            unsafe { t_dma_create_gpu_bo(size, T_RIGHT_READ | T_RIGHT_WRITE | T_RIGHT_MAP) };
        if fd < 0 {
            return false;
        }
        let va = self.weave_va_next;
        self.weave_va_next += (size + PAGE - 1) & !(PAGE - 1);
        let pa = unsafe { t_dma_map(fd, va, T_PROT_READ | T_PROT_WRITE) };
        if pa < 0 {
            unsafe { t_close(fd) };
            return false;
        }

        self.res_seq += 1;
        let res_id = self.res_seq;
        if self
            .gpu
            .resource_create_3d(res_id, target, format, bind, w, h, d, array, last_level, samples, flags)
            .is_err()
        {
            unsafe { t_close(fd) };
            return false;
        }
        if self.gpu.ctx_attach_resource(dev_ctx, res_id).is_err() {
            let _ = self.gpu.resource_unref(res_id);
            unsafe { t_close(fd) };
            return false;
        }
        if self.gpu.attach_backing(res_id, pa as u64, size as u32).is_err() {
            let _ = self.gpu.ctx_detach_resource(dev_ctx, res_id);
            let _ = self.gpu.resource_unref(res_id);
            unsafe { t_close(fd) };
            return false;
        }

        let c = self.wctx_mut(ctx_pub, conn).unwrap();
        for b in c.bos.iter_mut().flatten() {
            if b.pub_id == bo_pub {
                b.res_id = res_id;
                b.dma_fd = fd;
                b.va = va;
                b.pa = pa as u64;
                b.size = size;
                b.w = w;
                b.h = h;
                return true;
            }
        }
        false
    }

    /// The BO Tweft mint (the weft_ensure precedent): share once, echo the
    /// stored id thereafter.
    fn wbo_weft_ensure(&mut self, bo_pub: u32, conn: u64) -> Option<(u64, u32)> {
        for c in self.warp_ctxs.iter_mut().flatten() {
            if c.owner_conn != conn {
                continue;
            }
            for b in c.bos.iter_mut().flatten() {
                if b.pub_id != bo_pub {
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
    fn wbo_retire(gpu: &mut Gpu, dev_ctx: u32, b: &mut WarpBo, leak: bool) {
        if let Some(id) = b.share_id.take() {
            let _ = unsafe { t_weft_unshare(id) };
        }
        if b.dma_fd >= 0 {
            let _ = gpu.detach_backing(b.res_id);
            let _ = gpu.ctx_detach_resource(dev_ctx, b.res_id);
            let _ = gpu.resource_unref(b.res_id);
            if leak {
                say!("tapestryd: warp bo res {} LEAKED (fence wedge)", b.res_id);
            } else {
                unsafe { t_burrow_detach(b.va, b.size) };
                unsafe { t_close(b.dma_fd) };
            }
            b.dma_fd = -1;
        }
    }

    /// Retire one ctx: drain its fences (W2d), every BO, then CTX_DESTROY
    /// (synchronous, so the device-side ctx is quiesced when the slot --
    /// and with it the dev_ctx id -- becomes reusable; on the wedge/leak
    /// path the destroy is attempted anyway and fails fast if the engine
    /// latched dead).
    fn wctx_retire(&mut self, slot: usize) {
        let ctx_pub = match &self.warp_ctxs[slot] {
            Some(c) => c.pub_id,
            None => return,
        };
        let drained = self.warp_drain_ctx_fences(ctx_pub);
        if let Some(mut c) = self.warp_ctxs[slot].take() {
            for b in c.bos.iter_mut().flatten() {
                Self::wbo_retire(&mut self.gpu, c.dev_ctx, b, !drained);
            }
            let _ = self.gpu.ctx_destroy(c.dev_ctx);
        }
    }

    /// Retire a specific owned BO (the bo ctl `destroy` verb). W2d: an
    /// in-flight fenced chain may reference THIS BO (streams are scoped to
    /// ctx-attached resources), so the owning ctx drains first.
    fn wbo_destroy(&mut self, bo_pub: u32, conn: u64) -> bool {
        let ctx_pub = match self.wbo(bo_pub, conn) {
            Some((c, _)) => c.pub_id,
            None => return false,
        };
        let drained = self.warp_drain_ctx_fences(ctx_pub);
        for i in 0..MAX_WARP_CTXS {
            let (dev_ctx, owner) = match &self.warp_ctxs[i] {
                Some(c) => (c.dev_ctx, c.owner_conn),
                None => continue,
            };
            if owner != conn {
                continue;
            }
            let c = self.warp_ctxs[i].as_mut().unwrap();
            for j in 0..MAX_WARP_BOS_PER_CTX {
                if c.bos[j].as_ref().map_or(false, |b| b.pub_id == bo_pub) {
                    let mut b = c.bos[j].take().unwrap();
                    Self::wbo_retire(&mut self.gpu, dev_ctx, &mut b, !drained);
                    return true;
                }
            }
        }
        false
    }

    /// Conn teardown, warp half: retire every ctx this conn owns.
    fn warp_retire_conn(&mut self, conn: u64) {
        for i in 0..MAX_WARP_CTXS {
            if self.warp_ctxs[i]
                .as_ref()
                .map_or(false, |c| c.owner_conn == conn)
            {
                self.wctx_retire(i);
            }
        }
    }

    fn warp_live_ctxs(&self) -> usize {
        self.warp_ctxs.iter().flatten().count()
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
                if c.pub_id == tag.ctx_pub {
                    c.fences_in_flight = c.fences_in_flight.saturating_sub(1);
                    if tag.fence_id > c.fence_signaled {
                        c.fence_signaled = tag.fence_id;
                    }
                    break;
                }
            }
        }
    }

    pub fn warp_fences_pending(&self) -> bool {
        self.gpu.fenced_in_flight() > 0
    }

    /// Bounded pre-retire fence drain: an in-flight fenced chain DMAs
    /// guest pages, so a BO retired under one would free memory the host
    /// still writes. False = the drain deadlined or the engine is dead --
    /// the caller LEAKS the backing instead (never UAF). No deadline
    /// latches `dead` here: a slow fence is not a dead engine (the #31
    /// false-latch lesson); the sync path carries its own honest deadline.
    fn warp_drain_ctx_fences(&mut self, ctx_pub: u32) -> bool {
        const DRAIN_DEADLINE_MS: u64 = 2000;
        let mut t0: Option<Instant> = None;
        loop {
            self.warp_service_fences();
            let busy = self
                .warp_ctxs
                .iter()
                .flatten()
                .any(|c| c.pub_id == ctx_pub && c.fences_in_flight > 0);
            if !busy {
                return true;
            }
            if self.gpu.engine_dead() {
                return false;
            }
            let t = *t0.get_or_insert_with(Instant::now);
            if t.elapsed().as_millis() as u64 >= DRAIN_DEADLINE_MS {
                say!(
                    "tapestryd: warp ctx {} fence drain deadlined; leaking its backings",
                    ctx_pub
                );
                return false;
            }
            // The GPU IRQ is waitable only inside a sync submit (the G-5
            // engine owns it); pace the re-poll cooperatively.
            let _ = t_yield();
        }
    }

    /// SUBMIT_3D from the ctx submit file: one Twrite = one atomic opaque
    /// submission on the fenced lane. Returns the fence id (the fence
    /// file's completion record).
    fn warp_submit(&mut self, ctx_pub: u32, conn: u64, stream: &[u8]) -> Result<u64, u32> {
        let dev_ctx = self.wctx(ctx_pub, conn).ok_or(p9::E_NOENT)?.dev_ctx;
        match self.gpu.submit_3d(dev_ctx, ctx_pub, stream) {
            Ok(f) => {
                self.wctx_mut(ctx_pub, conn).unwrap().fences_in_flight += 1;
                Ok(f)
            }
            Err(e) => Err(map_fenced_err(e)),
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
        let (ctx_pub, dev_ctx, res_id, built) = match self.wbo(bo_pub, conn) {
            Some((c, b)) => (c.pub_id, c.dev_ctx, b.res_id, b.dma_fd >= 0),
            None => return Err(p9::E_NOENT),
        };
        if !built {
            return Err(p9::E_INVAL);
        }
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
                    "virgl {}\ncapsets {}\ncapset {} {} {}\nctxs {}\n",
                    comp.gpu.virgl as u32,
                    comp.gpu.num_capsets,
                    comp.gpu.capset_id,
                    comp.gpu.capset_ver,
                    comp.gpu.capset_blob.len(),
                    comp.warp_live_ctxs()
                ),
            );
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
                    let _ = core::fmt::write(&mut s, format_args!("{}\n", id));
                    self.read_str(tag, &s, a.offset, cap)
                }
                // The fence completion stream (W2d): one record per read
                // -- the newest retired fence id, coalescing (FIFO within
                // our single ring, so id N retires everything <= N); parks
                // when nothing is unreported (the FK_EVENT netd leg).
                // Offset is ignored: a stream, not a file image.
                WFK_FENCE => {
                    if cap < FENCE_REC_MAX {
                        // Too small for a whole record: answer empty rather
                        // than park a read that could never complete.
                        return p9::build_rread(&mut self.out_buf, tag, &[]);
                    }
                    let c = comp.wctx(id, self.conn_id).unwrap();
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
        s == "test-mode on" || s == "test-mode off" || s == "tick" || s.starts_with("release")
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
        if s.starts_with("test-mode") || s == "tick" || s.starts_with("release") {
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
            // The deferred direct switch (F16: SET_SCANOUT only at a
            // present-COMPLETE). A stale client resource (composed-era
            // presents never transferred to it) expands this transfer to
            // the full surface first.
            let stale = comp.surf(n).map_or(false, |s| s.res_stale);
            let xfer: Vec<(u32, u32, u32, u32)> =
                if stale { alloc::vec![(0, 0, w, h)] } else { rects.clone() };
            for &(tx, ty, tw, th) in &xfer {
                let offset =
                    (slot as u64) * slot_stride + ((ty as u64) * (w as u64) + tx as u64) * 4;
                if comp.gpu.transfer(res, offset, tx, ty, tw, th).is_err() {
                    return Err(E_IO);
                }
                if comp.gpu.flush(res, tx, ty, tw, th).is_err() {
                    return Err(E_IO);
                }
            }
            say!("tapestryd: scanout direct {} ({}x{})", n, w, h);
            if comp.gpu.set_scanout(res, w, h).is_ok() {
                // Post-bind full flush (#57): the per-rect flushes above
                // targeted a not-yet-scanned-out resource (dropped by
                // spec), and cocoa's same-size surface replace renders
                // nothing -- without this the display keeps the stale
                // composed frame until later client damage covers it
                // (the lingering-dead-pane symptom).
                let _ = comp.gpu.flush(res, 0, 0, w, h);
                comp.scanout = Scanout::Direct(n);
                comp.pending_direct = None;
                if let Some(s) = comp.surf_mut(n) {
                    s.res_stale = false;
                }
            }
        } else if comp.scanout == Scanout::Direct(n) {
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
            // Composed: blit the damage into the screen buffer at the
            // pane's content rect (a hidden/unhosted surface skips); the
            // client resource is now stale for a future direct switch.
            // Held presents blit NOW (weave bytes read only inside this
            // dispatch) and defer only the screen push.
            let mut acc: Option<Rect> = None;
            for &(x, y, pw, ph) in &rects {
                if let Some(r) = comp.blit_composed_pixels(n, slot, x, y, pw, ph) {
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
                    if c.owner_conn == self.conn_id {
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
                    for b in c.bos.iter().flatten() {
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
