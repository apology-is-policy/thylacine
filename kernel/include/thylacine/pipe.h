// Kernel pipe — connected pair of Spoors sharing a ring buffer (P5-pipe).
//
// Per ARCHITECTURE.md §10.3. Plan 9 `pipe(fd[2])` returns a Spoor pair
// connected by a kernel-internal byte FIFO. The primitive backs:
//   - The P5-spoor-transport adapter's production byte-pipe Spoor pair
//     (replacing the test scaffold in test_9p_spoor_transport.c).
//   - The future P5-stratumd boot path (kernel + stratumd talk through
//     a pipe pair until vsock or Unix sockets land).
//   - The eventual shell pipeline primitive (when userspace lands the
//     pipe(2) syscall).
//
// Semantics at v1.0 (non-blocking):
//   - read returns bytes available (0..n); 0 if empty.
//   - write returns bytes accepted (0..n); 0 if full.
//   - Neither end blocks. The non-blocking discipline is sufficient for
//     all v1.0 in-kernel uses (single-CPU, synchronous test sequencing,
//     pre-staged frame writes).
//   - Blocking semantics (read waits for data; write waits for room)
//     with rendez integration land at P5-pipe-blocking. That chunk
//     needs a spec extension (the missed-wakeup hazard, ARCH §28 I-9,
//     enters scope when wait/wake is wired up).
//
// Lifecycle:
//   - pipe_create allocates one shared ring + two Spoors. The ring has
//     refcount = 2 (one per Spoor).
//   - Each Spoor's close hook decrements the ring's refcount. The ring
//     is freed when both endpoints have been clunked.
//   - The Spoors are independently refcounted — clunking one does NOT
//     close the other, but does drop the ring's per-endpoint ref.
//
// Dev character: '|' (matches Plan 9 9front devpipe + shell pipe glyph).

#ifndef THYLACINE_PIPE_H
#define THYLACINE_PIPE_H

#include <thylacine/types.h>

struct Dev;
struct Spoor;

extern struct Dev devpipe;

#define DEVPIPE_DC          '|'
#define PIPE_BUF_SIZE       4096u           // POSIX PIPE_BUF guarantee
#define PIPE_RING_MAGIC     0x50495045u     // "PIPE" little-endian
#define PIPE_ENDPOINT_MAGIC 0x50494550u     // "PIEP" little-endian

// Bring up the pipe subsystem. Registers devpipe in the bestiary +
// allocates the SLUB caches for the ring + endpoint structs. Called
// from kernel/main.c after dev_init() (which has already run
// spoor_init).
void pipe_init(void);

// Create a connected Spoor pair. Returns 0 on success; -1 on OOM.
//   *out_read_end:  drains the ring (dev->read returns bytes; dev->write returns -1).
//   *out_write_end: fills the ring (dev->write returns bytes; dev->read returns -1).
//
// On failure, *out_read_end and *out_write_end are NULL; no partial
// state remains (all-or-nothing). On success, caller owns both Spoors
// (ref=1 each); spoor_clunk on each releases the per-endpoint ring ref.
//
// The two Spoors share the same ring; the ring's storage outlives
// EITHER endpoint and is freed only when BOTH are clunked.
int pipe_create(struct Spoor **out_read_end, struct Spoor **out_write_end);

// Diagnostic counters (ring-level; one ring per pipe pair).
u64 pipe_total_allocated(void);
u64 pipe_total_freed(void);

#endif  // THYLACINE_PIPE_H
