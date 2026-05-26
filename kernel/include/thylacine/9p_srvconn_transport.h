// 9P byte-mode-SrvConn transport adapter (P6-pouch-stratumd-boot 16c).
//
// Mirrors `kernel/9p_spoor_transport.{c,h}` -- the second concrete
// `p9_transport_ops` backend. Wraps a byte-mode `struct SrvConn`'s
// `c2s` (kernel-client -> server) and `s2c` (server -> kernel-client)
// byte rings into the send / recv / close vtable that
// `p9_transport_round_trip` drives.
//
// Layering above the existing transport core:
//
//     p9_client            (composes session + transport)
//        |
//     p9_session           (state machine + tag pool + fid table)
//        |
//     p9_transport         (frame discipline + state machine)
//        |
//     p9_transport_ops     (vtable; backend-agnostic)
//        |
//     p9_srvconn_transport <- THIS layer: routes ops to SrvConn rings
//        |
//     struct SrvConn  ->   c2s/s2c srvconn_chan byte rings (the byte pipe)
//
// What this chunk produces is the adapter layer. It does NOT define a
// new spec module -- `specs/9p_client.tla`'s invariants compose
// through unchanged. The adapter is pure plumbing.
//
// =============================================================================
// The byte-mode requirement (audit-bearing invariant)
// =============================================================================
//
// The wrapped SrvConn MUST be byte-mode (`__atomic_load_n(&cn->byte_mode,
// ACQUIRE) == true`). 9P-mode SrvConns -- which `corvus` posts via
// `SYS_POST_SERVICE` -- already have an EMBEDDED kernel-owned p9_client
// driving Tread/Twrite on a single `client_fid` (the corvus-side verb
// stream). A second p9_client wrapped via this adapter would inject its
// own Tversion/Tattach/Twalk/etc frames into the SAME c2s/s2c rings,
// interleaving with the embedded client's frames and producing wire
// corruption.
//
// The byte-mode gate is enforced at the CONSUMER -- `SYS_ATTACH_9P_SRV`
// in `kernel/syscall.c::sys_attach_9p_srv_handler` rejects with -1 if
// the source SrvConn is 9P-mode. This adapter is defense-in-depth:
// it does NOT re-check the mode (the consumer already gated), but its
// header contract is unambiguous about the requirement.
//
// Pouch sockets (sub-chunk 12) post BYTE-mode via SYS_POST_SERVICE_BYTE,
// so stratumd's `/srv/stratum-fs` listener naturally produces byte-mode
// SrvConns -- the production path is consistent with the contract.
//
// =============================================================================
// Lifecycle ownership
// =============================================================================
//
// The adapter takes ONE srvconn_ref at init via `p9_srvconn_transport_init`
// and drops that ref via `srvconn_unref` in the `close` op. SrvConn
// refcount is the lifetime authority -- the underlying SrvConn lives as
// long as anyone holds a ref, including this adapter.
//
// Discipline: caller's reference (from the SYS_ATTACH_9P_SRV handler's
// `handle_get(srv_fd)` step) is taken INDEPENDENTLY before this adapter
// is constructed; the adapter's ref is ADDITIVE. So userspace closing
// the original srv_fd doesn't tear the SrvConn down while the adapter
// (held inside a `p9_attached`) is still using it. Matches the
// SYS_ATTACH_9P discipline for transport-Spoor refs.
//
// =============================================================================
// I/O semantics
// =============================================================================
//
// - send: writes go to c2s via `srvconn_client_send`. Non-blocking;
//   SRVCONN_RING_CAP (8 KiB) is sized to hold a full msize (4 KiB)
//   frame, so a synchronous single-frame-in-flight kernel 9P client
//   never blocks on a write. A short write (the ring filled because of
//   a bug -- should not happen) loops in spoor-transport style.
//
// - recv: reads block on s2c via `srvconn_client_recv`. Each blocking
//   recv is bounded by `cn->client_deadline_ns`; the caller must set
//   the deadline BEFORE each blocking op via
//   `srvconn_set_client_deadline`. SYS_ATTACH_9P_SRV does this for
//   the Tversion/Tattach handshake; subsequent operations (Twalk/
//   Tread/Twrite via the returned dev9p root) are driven by sys_read /
//   sys_write / sys_walk_open through the p9_client API, which set
//   their own deadlines through the same mechanism.
//
// - close: drops the adapter's srvconn_ref. Idempotent under
//   re-close (no-ops once the inner srvconn pointer is cleared).
//
// =============================================================================
// Concurrency
// =============================================================================
//
// Matches the underlying transport core -- NOT thread-safe internally.
// Callers serialize. At v1.0 the kernel 9P client serializes per-
// session via `p9_client.lock`, so at most one thread drives send/recv
// against the rings at a time. The wrapped SrvConn's per-direction
// channels each have a single blocking consumer (the kernel 9P client
// for s2c reads); the byte-mode-only adapter is the SOLE consumer of
// c2s writes + s2c reads.
//
// =============================================================================

#ifndef THYLACINE_9P_SRVCONN_TRANSPORT_H
#define THYLACINE_9P_SRVCONN_TRANSPORT_H

#include <thylacine/9p_transport.h>
#include <thylacine/types.h>

struct SrvConn;

#define P9_SRVCONN_TRANSPORT_MAGIC  0x50395343u   // "P9SC" little-endian

struct p9_srvconn_transport {
    u32             magic;     // P9_SRVCONN_TRANSPORT_MAGIC; clobbered on destroy
    struct SrvConn *cn;        // the wrapped SrvConn; NULL after close
};

// Initialize the adapter. Takes ONE srvconn_ref on `cn` so the SrvConn
// outlives the adapter (caller's reference -- via handle_get or
// equivalent -- stays independent). Returns 0 on success, -1 on NULL
// args. Pre-condition: `cn` must be byte-mode (`cn->byte_mode == true`);
// the adapter does NOT re-check (defense-in-depth check lives at
// SYS_ATTACH_9P_SRV).
int p9_srvconn_transport_init(struct p9_srvconn_transport *st,
                                struct SrvConn *cn);

// Tear down: clobbers magic, clears the inner pointer. Does NOT drop
// the srvconn_ref -- the close path is responsible (mirrors
// p9_spoor_transport_destroy's contract; spoor_clunk vs spoor_unref
// discipline lives in close).
void p9_srvconn_transport_destroy(struct p9_srvconn_transport *st);

// Build the transport_ops vtable backed by this adapter. The returned
// struct is by-value (the transport core copies it into its own state);
// the adapter's address is captured in ops.ctx.
//
// Adapter MUST outlive the transport that uses these ops.
struct p9_transport_ops p9_srvconn_transport_ops(struct p9_srvconn_transport *st);

// Query: is the adapter live (magic valid + inner SrvConn present)?
bool p9_srvconn_transport_is_open(const struct p9_srvconn_transport *st);

#endif  // THYLACINE_9P_SRVCONN_TRANSPORT_H
