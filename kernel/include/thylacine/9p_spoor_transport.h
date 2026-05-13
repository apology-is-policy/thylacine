// 9P Spoor-pair transport backend (P5-spoor-transport).
//
// Composes the kernel's Spoor abstraction (ARCH §9.2) with the 9P
// transport vtable (`<thylacine/9p_transport.h>`). Each adapter wraps a
// pair of Spoors — `tx_spoor` for outbound writes, `rx_spoor` for
// inbound reads — and exposes them through `struct p9_transport_ops`.
//
// The two Spoors may be the SAME Spoor for duplex byte pipes (Unix
// sockets, vsock, virtio-vsock) where one Spoor carries traffic in both
// directions. For separate read / write endpoints (Plan 9 `pipe(fd[2])`
// when it lands), pass two distinct Spoors.
//
// Layering above the existing transport core:
//
//     p9_client            (composes session + transport)
//        │
//     p9_session           (state machine + tag pool + fid table)
//        │
//     p9_transport         (frame discipline + state machine)
//        │
//     p9_transport_ops     (vtable; backend-agnostic)
//        │
//     p9_spoor_transport   ← THIS layer: routes ops to Spoor read/write
//        │
//     struct Spoor → struct Dev::read / write     (the byte pipe)
//
// What this chunk produces is the adapter layer. It does NOT define a
// new spec module — `specs/9p_client.tla`'s invariants compose through
// unchanged. The adapter is pure plumbing.
//
// Lifecycle ownership:
//   - `owns_spoors = false`: caller retains spoor_unref / spoor_clunk
//     responsibility. The adapter never touches Spoor refs. Use this
//     when the Spoors outlive the adapter (e.g., the Spoors are mounted
//     in a Territory, or pinned by a Burrow that outlives the
//     transport).
//   - `owns_spoors = true`: the adapter clunks both Spoors on close.
//     Use this for adapter-internal Spoor pairs (e.g., a kernel-
//     internal pipe pair created just for one 9P session).
//
// Concurrency: matches the underlying transport core — NOT thread-safe
// internally. Callers serialize.

#ifndef THYLACINE_9P_SPOOR_TRANSPORT_H
#define THYLACINE_9P_SPOOR_TRANSPORT_H

#include <thylacine/9p_transport.h>
#include <thylacine/types.h>

struct Spoor;

#define P9_SPOOR_TRANSPORT_MAGIC  0x50395354u   // "P9ST" little-endian

struct p9_spoor_transport {
    u32           magic;        // P9_SPOOR_TRANSPORT_MAGIC; clobbered on destroy
    struct Spoor *tx_spoor;     // outbound writes
    struct Spoor *rx_spoor;     // inbound reads (may equal tx_spoor)
    bool          owns_spoors;  // clunk on close if true
};

// Initialize the adapter. Returns 0 on success; -1 on NULL args.
//   tx / rx may be the same Spoor (duplex byte pipe).
//   owns_spoors: see lifecycle comments above.
int p9_spoor_transport_init(struct p9_spoor_transport *st,
                            struct Spoor *tx, struct Spoor *rx,
                            bool owns_spoors);

// Tear down: clobbers magic. Does NOT clunk Spoors here even if
// owns_spoors=true — the close path is responsible. Calling destroy
// on a still-OPEN adapter (without going through transport_close →
// adapter_close) leaks the Spoor refs; that's a caller-side bug
// caught by spoor_unref's underflow extinct at higher level.
void p9_spoor_transport_destroy(struct p9_spoor_transport *st);

// Build the transport_ops vtable backed by this adapter. The returned
// struct is by-value (the transport core copies it into its own state);
// the adapter's address is captured in ops.ctx.
//
// Adapter MUST outlive the transport that uses these ops.
struct p9_transport_ops p9_spoor_transport_ops(struct p9_spoor_transport *st);

// Query: is the adapter live (magic valid + at least tx_spoor present)?
bool p9_spoor_transport_is_open(const struct p9_spoor_transport *st);

#endif  // THYLACINE_9P_SPOOR_TRANSPORT_H
