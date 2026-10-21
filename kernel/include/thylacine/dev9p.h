// dev9p — Dev vtable proxying to a kernel 9P client (P5-attach-dev).
//
// Per ARCHITECTURE.md §9.6 "filesystem-as-Spoor". This Dev is the
// proxy that makes a remote 9P server look exactly like a kernel-
// internal filesystem: every Dev vtable op (walk / open / read /
// write / clunk / stat) routes through a `p9_client` instance to
// the server.
//
// Layering:
//
//   Spoor walk / open / read / write              ← kernel callers
//      │  (Dev vtable dispatch)
//   dev9p_walk / dev9p_open / dev9p_read / ...    (this module)
//      │
//   p9_client_walk_one / p9_client_lopen / ...
//      │
//   p9_session + p9_transport
//      │
//   {loopback, Spoor-over-Unix-socket, ...}
//
// Each `dev9p`-backed Spoor carries a `struct dev9p_priv` in its
// `aux` field with (client, fid) — the client pointer + the 9P fid
// this Spoor represents. Walk allocates a fresh fid via the client's
// monotonic allocator; close clunks the fid.
//
// Lifecycle ownership (v1.0):
//
//   - dev9p does NOT own the p9_client. The client is allocated +
//     destroyed by a higher layer (eventually the attach_9p syscall;
//     for now, tests).
//   - Each Spoor's aux carries the (client *, fid) pair. The pointer
//     is stable for the Spoor's lifetime; no refcounting on the
//     client struct itself.
//   - Spoor close → dev9p_close → clunk the fid + kfree the priv.
//   - When the higher layer destroys the client, ALL Spoors backed by
//     it must already be released. v1.0 enforces this by convention
//     (test discipline + the future attach_9p syscall tearing down
//     in the right order).
//
// The Dev struct is registered in the bestiary like any other Dev,
// but its `attach(spec)` slot returns NULL — dev9p Spoors are not
// constructed via the standard Dev attach path. Instead, kernel
// callers use `dev9p_attach_client(client, root_fid)` to construct
// the root Spoor of a 9P-mounted tree. (The attach_9p syscall will
// be the user-visible entry point in P5-attach-syscall.)

#ifndef THYLACINE_DEV9P_H
#define THYLACINE_DEV9P_H

#include <thylacine/types.h>
#include <thylacine/spinlock.h>  // spin_lock_t (the write-behind wb_lock)
#include <thylacine/syscall.h>   // struct t_stat (the cached-open co_stat field)

struct p9_client;
struct p9_attached;
struct p9_spoor_transport;
struct Spoor;
struct Dev;
struct poll_waiter;
struct dev9p_poll_state;   // net-6b-2b: lazily-allocated per-Spoor poll state (dev9p_poll.c)
struct weft_binding;       // Weft-6a-2: lazily-bound per-flow ring share (weft.c)
struct Path;               // the session root's origin name (path.h)

// Device character for dev9p — '9'. Distinct from all kernel-Dev
// characters (-, c, 0, z, r, p, C, m).
#define DEV9P_DC  '9'

// The dev9p Dev struct (vtable). Registered in the bestiary by
// dev9p_init (called from dev_init's startup sequence).
extern struct Dev dev9p;

// Per-Spoor private state. Allocated by dev9p_attach_client and by
// dev9p_walk; freed by dev9p_close.
//
// P5-stratumd-stub-bringup audit close F2 (F236 deferred close):
// `attached_owner` semantics extended — EVERY dev9p_priv derived from
// a SYS_ATTACH_9P session now carries a non-NULL attached_owner and
// holds one ref via p9_attached_ref. dev9p_close drops the ref; the
// last unref runs the attached's full teardown (client + transport +
// adapter). Walked privs propagate the parent's attached_owner pointer
// and bump the ref at priv_alloc. Pre-fix only the root carried this
// pointer, leaving walked privs dangling after the root's close ran
// p9_attached_destroy immediately (R15 F236).
//
// For test paths that construct a Spoor via dev9p_attach_client(client,
// root_fid) directly (no p9_attached wrapper), attached_owner stays
// NULL and dev9p_close skips the unref. The fid_owned semantics still
// apply: walks from those test roots clunk their fid as today.
struct dev9p_priv {
    u32                magic;
    struct p9_client  *client;     // pointer to the client; lifecycle managed externally
    u32                fid;        // the 9P fid this Spoor represents
    bool               fid_owned;  // true iff close should clunk; root Spoor's fid
                                   // (the one from p9_client_handshake) is NOT clunked
                                   // by dev9p — the higher layer manages it.
    // G2 (the dir-fid cache; docs/FID-LIFECYCLE-DESIGN.md section 4):
    // fid_gen is the Larder invalidation-gen snapshot at the fid's MINT (the
    // RPC bind tail or a dir-fid consume). dev9p_close parks an unopened dir
    // fid ONLY if no invalidation event has named this qid since fid_gen
    // (larder_qid_staled_since over the G4 ring) -- a checked-out fid whose
    // dir was rmdir'd/replaced mid-use must die, never re-park. fid_suspect
    // latches when a by-name op (create/unlink/rename/wstat) ERRORS through
    // this priv -- the backstop that breaks the stale-fid re-park loop when
    // the staleness event was unknowable (the evicted-dentry residual): a fid
    // that misbehaved once is clunked at close, never re-served.
    u64                fid_gen;
    bool               fid_suspect;
    // #99 (#102 errno-loss): the last dev9p_create failure's real errno, in the
    // syscall passthrough range (e.g. -T_E_EXIST on a racing/duplicate create).
    // Transient create-path state: born 0 (kzalloc), set by dev9p_create on a
    // Rlerror, read once by sys_walk_create_handler via dev9p_create_errno()
    // immediately after a NULL create -- BEFORE the handler-local `nc` is clunked
    // (freeing this priv). Never shared: `nc` is a fresh clone-walk, not yet a
    // handle. 0 means "not recorded" -> the accessor returns -1 (prior behavior).
    int                create_errno;
    // Dev.open also returns a pointer, so preserve its Rlerror on the fresh,
    // unpublished walk result. Read before clunk; reset on every open attempt.
    int                open_errno;
    // F2: attached_owner is the session-resource holder. NULL when the
    // p9_client is externally owned (test path); non-NULL for every priv
    // derived from a SYS_ATTACH_9P session (root + walks). Each non-NULL
    // owner contributes one p9_attached_ref; dev9p_close drops it.
    struct p9_attached       *attached_owner;
    // The file this session came over (operator vote 2026-09-28; ARCH 9.6.9),
    // on the session ROOT alone: stamped by dev9p_stamp_origin from the attach
    // handler before the root is published, never changed after. origin holds a
    // ref on that file's name; origin_dc is its device char when it has none (a
    // pipe). Display only (I-33) -- territory_format_ns is the one reader. A
    // walk gives its result a fresh priv, so no walked Spoor carries one.
    struct Path              *origin;
    char                      origin_dc;
    // net-6b-2b: lazily-allocated poll state for a QTPOLL (netd `ready`) Spoor;
    // NULL for every regular dev9p file (the common path). Allocated by the first
    // readiness ARM of a readiness file (dev9p_poll_arm; a snapshot needs none).
    // #294: independently REFCOUNTED -- the priv holds one ref (dropped via
    // dev9p_poll_priv_release at dev9p_close), each outstanding arm holds one;
    // freed when both drop (so an arm the kthread still owns keeps it alive after
    // this priv frees). Owned by THIS priv (not shared across walks -- each walked
    // Spoor gets its own priv).
    struct dev9p_poll_state  *poll;
    // Weft-6a-2: lazily-bound per-flow ring share for a /net data fid; NULL for
    // every fd that has not gone zero-copy (the common path). Installed by
    // SYS_WEFT_MAP on the first large transfer (holds the I-30 registration pin);
    // released by dev9p_close. Multi-thread-reachable (a data fd is handle_dup-
    // shareable), so SYS_WEFT_MAP installs it via an __atomic CAS + reads it
    // __atomic-acquire; dev9p_close reads it plainly (it runs at the LAST ref, so
    // no concurrent mapper exists).
    struct weft_binding      *weft;
    // FID-LIFECYCLE cached-open (docs/FID-LIFECYCLE-DESIGN.md section 3.3): the
    // fidless open's per-open state. cached_open == true implies fid == P9_NOFID
    // + fid_owned == false; co_buf holds the [0, co_size) content snapshot taken
    // at open (NULL iff co_size == 0 -- the empty-file open), immutable for the
    // Spoor's lifetime (reads copy out with no lock); co_stat is the open-time
    // metadata (serves fstat -- the same close-to-open discipline as the attr
    // cache). dev9p_close frees co_buf + uncharges the global budget.
    bool                      cached_open;
    u8                       *co_buf;
    u64                       co_size;
    struct t_stat             co_stat;
    // F1 write-behind (LARDER-DESIGN section 12): the per-open-file append-run
    // staging state. wb_lock is a PURE LEAF (only byte copies + state reads
    // under it -- wire I/O never; nothing else is ever acquired under it).
    // The run is the single contiguous byte range [wb_off, wb_off + wb_len)
    // held in wb_buf; wb_base is the append ANCHOR (the file's known current
    // end -- valid iff wb_known, born 0 at create/OTRUNC, advanced by
    // completed flushes + completed write-throughs). wb_flushers counts
    // in-flight flushes: while nonzero the run is FROZEN (no stage, no growth
    // realloc -- a flusher reads wb_buf outside the lock) and concurrent
    // writes go write-through; readers overlay the still-visible run.
    // wb_err latches the first flush failure (positive errno); once set,
    // every subsequent write/fsync on this fd, and its last close, returns
    // it (the voted NFS error model) and the run is dropped.
    spin_lock_t               wb_lock;
    bool                      wb_eligible;  // create/OTRUNC-born + loose+cacheable plain file
    bool                      wb_known;     // wb_base is the file's true current end
    u32                       wb_flushers;  // in-flight flush count (freezes the run)
    int                       wb_err;       // latched flush errno (0 = none)
    u8                       *wb_buf;       // staged bytes (budget-charged wb_cap)
    u32                       wb_cap;       // wb_buf allocation size
    u32                       wb_len;       // staged byte count
    u64                       wb_off;       // run start offset
    u64                       wb_base;      // append anchor (valid iff wb_known)
};

// Cached-open bounds (FID-LIFECYCLE section 3.3; the CF-3 bounce-budget class --
// the snapshot is user-drivable kernel heap). Per-file cap: beyond it the fid
// RTs amortize against the read volume anyway (the target is the small go-cache
// file, ~1.6 pages measured). Global outstanding-bytes budget: GLOBAL, not
// per-Proc -- a cached-open fd crosses Proc boundaries (rfork inheritance,
// handle transfer), so a per-Proc charge would unbalance at close-by-inheritor.
// Exhaustion degrades the fast path only (the fallback is the normal open).
#define DEV9P_CO_MAX_SIZE  (128u * 1024u)
#define DEV9P_CO_BUDGET    (8u * 1024u * 1024u)

// Diagnostics: bytes currently held by live cached-open snapshots (the global
// budget's occupancy). Tests assert the charge/uncharge balance.
u64 dev9p_co_budget_used(void);

// Write-behind bounds (F1, LARDER-DESIGN section 12; the DEV9P_CO_* class).
// Per-run buffer cap: two msize payloads -- steady-state staging turns the
// measured 4-KiB write dribble into full-msize wire writes. Stage-size cap:
// a bigger write is already wire-efficient and would stretch the copy held
// under the priv spinlock -- it flushes the run then writes through. Global
// outstanding-staged-bytes budget: GLOBAL, not per-Proc -- the priv crosses
// Proc boundaries (handle_dup / rfork inheritance / the #926 close-at-exit
// runs in whichever holder dies last), so a per-Proc charge would unbalance
// at close-by-inheritor, exactly the DEV9P_CO_BUDGET reasoning above.
// Exhaustion degrades to write-through (the strict-mount behavior).
#define DEV9P_WB_CAP        (256u * 1024u)
#define DEV9P_WB_STAGE_MAX  (32u * 1024u)
#define DEV9P_WB_BUDGET     (8u * 1024u * 1024u)

// Diagnostics: bytes currently held by live staged-run buffers (the global
// wb budget's occupancy). Tests assert the charge/uncharge balance.
u64 dev9p_wb_budget_used(void);

// Test-only: bias the wb budget occupancy by +/-n (a signed add). Lets the
// budget-denial fallback be exercised without minting DEV9P_WB_BUDGET worth
// of live runs. Balanced add/subtract pairs only; never in production paths.
void dev9p_wb_budget_bias_for_test(s64 n);

#define DEV9P_PRIV_MAGIC 0x44395050u   // "D9PP" little-endian

// Initialize the dev9p subsystem. Idempotent guard extincts on
// re-call. Must run after spoor_init + dev_init (registers dev9p in
// the bestiary). Called from boot bring-up in `kernel/main.c`.
void dev9p_init(void);

// Construct the root Spoor of a 9P-mounted tree.
//
// Returns a new Spoor backed by dev9p whose aux = (client, root_fid).
// `client` must be in OPEN state (handshake completed; root_fid
// bound). The returned Spoor's `fid_owned` is FALSE — closing this
// Spoor does NOT clunk root_fid (the caller manages the root fid +
// client lifecycle).
//
// Subsequent walks through this Spoor allocate fresh fids via
// `p9_client_alloc_fid`; those walk-derived Spoors have
// `fid_owned = true` and clunk their fids on close.
//
// Returns NULL on:
//   - client == NULL or not in OPEN state
//   - SLUB OOM (spoor_alloc or kmalloc)
struct Spoor *dev9p_attach_client(struct p9_client *client, u32 root_fid);

// Resolve a dev9p-backed Spoor to its (p9_client, 9P fid) -- the Loom
// submit-time pin (I-30; docs/LOOM.md §8.5) reads these to dispatch an async op
// directly to the engine. Returns 0 on success (*out_client + *out_fid set), -1
// if `c` is not a dev9p Spoor (dc != DEV9P_DC) or its priv is missing/corrupt.
// The returned client pointer is valid only while the caller holds a ref on `c`
// (a live dev9p Spoor implies a live client -- dev9p's lifecycle invariant).
int dev9p_client_fid(struct Spoor *c, struct p9_client **out_client, u32 *out_fid);

// The dev9p side of a Loom registration: a staged write-behind run is flushed,
// the Spoor stops staging and its Larder pages are dropped, so no Loom op
// meets bytes still staged. May wait for the server. 0 (also for a non-dev9p
// Spoor), or a negative errno: one latched by an earlier flush, or this
// flush's, with the run still staged when a death ended it.
int dev9p_loom_register(struct Spoor *c);

// Weft-6b-2 data drive: try the zero-copy write path for a /net data fd whose
// SYS_WRITE buffer points INTO its weft-bound shared ring. The kernel validates
// the descriptor against the flow's private ring view (the I-30 validator-once)
// and issues Tweftio(WRITE); netd reads the ring in place + replies the count.
// Returns 1 if handled (*accepted = bytes moved), 0 if NOT a weft write (the
// caller falls back to the byte-copy path), -1 on a weft transport error. Called
// from the write syscall handler with the caller's user VA (before any copy-in).
int dev9p_weft_try_write(struct Spoor *spoor, u64 ubuf_va, u32 len, u32 *accepted);

// Weft-6b-3 data drive (RX): try the zero-copy read path for a /net data fd whose
// SYS_READ buffer points INTO its weft-bound shared ring. The kernel validates the
// destination descriptor against the flow's private ring view (the I-30
// validator-once) and issues Tweftio(READ); netd recvs IN PLACE into the ring +
// replies the count. Returns 1 if handled (*got = bytes recv'd, written directly
// into the guest's ring), 0 if NOT a weft read (the caller falls back to the
// byte-copy path), -1 on a weft transport error. Called from the read syscall
// handler with the caller's user VA; on a handled read the handler does NO
// uaccess_store (netd already wrote the bytes into the guest's shared mapping).
int dev9p_weft_try_read(struct Spoor *spoor, u64 ubuf_va, u32 len, u32 *got);

// Resolve a dev9p-backed Spoor to its `struct dev9p_priv *` (dc + magic gated;
// NULL if `c` is not a live dev9p Spoor). Exposed for kernel/dev9p_poll.c (the
// readiness bridge reads p->poll + p->client + p->fid) + the dev9p_poll tests.
struct dev9p_priv *dev9p_priv_of(struct Spoor *c);

// LR-1 (HAUL-DESIGN 4.8): does `c` belong to a 9P session whose attacher or
// /srv poster declared it remote? A lock-free read of a flag stamped before the
// session's root published; the caller's reference on `c` keeps its priv and
// client alive. False for anything that is not a dev9p Spoor with a valid priv.
// Read by territory_format_ns (the label) and, as dev9p's Dev.remote, by the
// resolver, which contains a link the session serves beneath the mount it was
// reached through (DISTRO 4.6) -- a narrowing; the declaration grants nothing.
bool dev9p_spoor_remote(struct Spoor *c);

// Name a session ROOT by the file its session came over (operator vote
// 2026-09-28): `transport`'s namespace name, else its device char. The attach
// handlers call it between minting the root and publishing it (I-33's
// set-before-publish). Only an unstamped root takes it -- a walked or
// cached-open priv is left alone -- and it cannot fail: the name is shared by
// reference, never copied.
void dev9p_stamp_origin(struct Spoor *root, const struct Spoor *transport);

// The name dev9p_stamp_origin gave a session root: true with *name (borrowed;
// alive while the caller holds `c`), or with *name NULL and *dc the file's
// device char. False for anything else. Its one caller is territory_format_ns.
bool dev9p_spoor_origin(struct Spoor *c, const struct Path **name, char *dc);

// #99: the create errno accessor for sys_walk_create_handler. Returns the errno
// dev9p_create recorded for the last create failure on this Spoor -- clamped to
// the syscall passthrough range [-4095, -2] so it reaches EL0 as the true POSIX
// errno (e.g. -T_E_EXIST). Self-gating: returns -1 (the prior generic-failure
// sentinel) for a non-dev9p Spoor, an unrecorded value, or an out-of-range one.
s64 dev9p_create_errno(struct Spoor *c);
// Failed Tlopen cause in [-4095,-2], otherwise -1; only before clunk.
s64 dev9p_open_errno(struct Spoor *c);

// =============================================================================
// dev9p's remote readiness -- the SAMPLE/ARM bridge (net-6b-2b, #98; NET-DESIGN
// section 12.2, specs/net_poll.tla). Defined in kernel/dev9p_poll.c.
// =============================================================================

struct poll_snap;

// The Dev `.poll_snapshot` slot (<thylacine/dev.h> documents the contract). For a
// QTPOLL file on a deadline-capable client it sends the snapshot read (offset =
// the requested events | P9_POLL_SNAPSHOT), which the server answers at once;
// any other dev9p file is answered here, POSIX always-ready
// (`events & POLL_REQUESTABLE`).
void dev9p_poll_snapshot(struct Spoor *c, short events, struct poll_snap *s);

// The Dev `.poll_snapshot_release` slot: flush the snapshot if unanswered (the
// c->lock barrier), unlink it, free it.
void dev9p_poll_snapshot_release(struct Spoor *c, struct poll_snap *s);

// The Dev `.poll_arm` slot: register `pw` on the Spoor's poll-state hook list,
// then ensure an arm (offset = the events) covering them is on the wire. 1 =
// armed; 0 = a shortage (no memory, no free tag, a full send ring) or a dead
// session left the file uncovered.
int dev9p_poll_arm(struct Spoor *c, short events, struct poll_waiter *pw);

// Initialize the global poll-pump registry + lock + kthread rendez. Idempotent.
// Call once at boot, before spawning the pump kthread.
void dev9p_poll_init(void);

// The global poll-pump kthread entry (the cons_poll console_mgr + Loom-4 SQPOLL
// analog). Spawned once at boot via thread_create(kproc(), dev9p_poll_pump_main).
// Drives the 9P elected reader for every client with an arm or a snapshot out
// (borrowing the client through a session ref), walks the poll-state hook lists
// when an arm is answered (in process context), reaps answered arms, and
// garbage-collects stranded ones. Snapshots are their pollers' to release.
void dev9p_poll_pump_main(void);

// Release a priv's poll state at dev9p_close (#294 cancel-at-close,
// specs/net_poll_teardown.tla). A registered poller holds the Spoor obj-ref, so
// poll_list is empty at close, but an outstanding readiness op may still be live
// (it pins the refcounted poll-state + the session, NOT the Spoor). This grabs +
// cancels that op at the client (Tflush) -- so the subsequent `ready`-fd Tclunk in
// dev9p_close frees the netd slot deterministically without orphaning the held
// readiness Tread -- and drops the priv's poll-state ref.
void dev9p_poll_priv_release(struct dev9p_priv *p);

// Test accessors (test_dev9p): the arm registry's length, and the snapshots still
// linked. Let a test assert a teardown (a count back to its baseline) without
// exposing the static registries.
u32 dev9p_poll_op_count_for_test(void);
u32 dev9p_poll_snap_count_for_test(void);

// Test accessor: the poll-pump kthread is asleep on its park (so it is pumping no
// client). A test waits for it before destroying a client the kthread served.
bool dev9p_poll_parked_for_test(void);

// Test hook: the poll collector's mode. dev9p_poll_test_gc_release is the
// runner's release after every test (back to RUN; whether it was left set).
enum dev9p_poll_test_gc_mode {
    DEV9P_POLL_GC_RUN  = 0,   // collect stranded arms (production)
    DEV9P_POLL_GC_SKIP = 1,   // leave them for a close to cancel
    DEV9P_POLL_GC_HOLD = 2,   // stop between the collect and the frees
};
void dev9p_poll_test_gc(enum dev9p_poll_test_gc_mode mode);
bool dev9p_poll_gc_held_for_test(void);
bool dev9p_poll_test_gc_release(void);

#endif  // THYLACINE_DEV9P_H
