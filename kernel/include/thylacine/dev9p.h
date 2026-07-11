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
#include <thylacine/syscall.h>   // struct t_stat (the cached-open co_stat field)

struct p9_client;
struct p9_attached;
struct p9_spoor_transport;
struct Spoor;
struct Dev;
struct poll_waiter;
struct dev9p_poll_state;   // net-6b-2b: lazily-allocated per-Spoor poll state (dev9p_poll.c)
struct weft_binding;       // Weft-6a-2: lazily-bound per-flow ring share (weft.c)

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
    // F2: attached_owner is the session-resource holder. NULL when the
    // p9_client is externally owned (test path); non-NULL for every priv
    // derived from a SYS_ATTACH_9P session (root + walks). Each non-NULL
    // owner contributes one p9_attached_ref; dev9p_close drops it.
    struct p9_attached       *attached_owner;
    // net-6b-2b: lazily-allocated poll state for a QTPOLL (netd `ready`) Spoor;
    // NULL for every regular dev9p file (the common path). Allocated by dev9p_poll
    // on the first poll of a readiness file. #294: independently REFCOUNTED -- the
    // priv holds one ref (dropped via dev9p_poll_priv_release at dev9p_close), each
    // outstanding readiness op holds one; freed when both drop (so an op the kthread
    // still owns keeps it alive after this priv frees). Owned by THIS priv (not
    // shared across walks -- each walked Spoor gets its own priv).
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
// `.poll` bridge reads p->poll + p->client + p->fid) + the dev9p_poll tests.
struct dev9p_priv *dev9p_priv_of(struct Spoor *c);

// =============================================================================
// dev9p.poll -- the readiness bridge (net-6b-2b; NET-DESIGN section 12.2,
// specs/net_poll.tla). Defined in kernel/dev9p_poll.c.
// =============================================================================

// The Dev `.poll` slot. For a netd `ready` file (cached qid.type carries QTPOLL)
// it registers `pw` on the Spoor's poll-state hook list + ensures an outstanding
// async readiness Tread (offset = the event mask) is in flight, then samples the
// last-known revents; for any other (regular) dev9p file it is POSIX always-ready
// (`events & POLL_REQUESTABLE`, the prior NULL-slot behavior). Returns the ready
// `revents` subset (>= 0). `pw == NULL` is the sample-only re-scan (no register).
short dev9p_poll(struct Spoor *c, short events, struct poll_waiter *pw);

// Initialize the global poll-pump registry + lock + kthread rendez. Idempotent.
// Call once at boot, before spawning the pump kthread.
void dev9p_poll_init(void);

// The global poll-pump kthread entry (the cons_poll console_mgr + Loom-4 SQPOLL
// analog). Spawned once at boot via thread_create(kproc(), dev9p_poll_pump_main).
// Drives the 9P elected reader for outstanding readiness ops (borrowing the
// client from a live op's pin), walks the poll-state hook lists on completion (in
// process context), reaps terminal ops, and garbage-collects stranded ops.
void dev9p_poll_pump_main(void);

// Release a priv's poll state at dev9p_close (#294 cancel-at-close,
// specs/net_poll_teardown.tla). A registered poller holds the Spoor obj-ref, so
// poll_list is empty at close, but an outstanding readiness op may still be live
// (it pins the refcounted poll-state + the session, NOT the Spoor). This grabs +
// cancels that op at the client (Tflush) -- so the subsequent `ready`-fd Tclunk in
// dev9p_close frees the netd slot deterministically without orphaning the held
// readiness Tread -- and drops the priv's poll-state ref.
void dev9p_poll_priv_release(struct dev9p_priv *p);

// Test accessor (test_dev9p): the live readiness-op registry length. Lets a test
// assert the cancel-at-close teardown (op count back to baseline) without exposing
// the static registry. Reads the atomic count; no lock needed.
u32 dev9p_poll_op_count_for_test(void);

#endif  // THYLACINE_DEV9P_H
