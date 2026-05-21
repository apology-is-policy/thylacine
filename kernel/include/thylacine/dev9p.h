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

struct p9_client;
struct p9_attached;
struct p9_spoor_transport;
struct Spoor;
struct Dev;

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
};

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

#endif  // THYLACINE_DEV9P_H
