// Kernel pipe — connected pair of Spoors sharing a ring buffer (P5-pipe).
//
// Per ARCHITECTURE.md §10.3. Plan 9 `pipe(fd[2])` returns a Spoor pair
// connected by a kernel-internal byte FIFO. It backs SYS_PIPE (shell
// pipelines, musl's pipe(2)) and the 9P spoor-transport adapter's byte-pipe
// pair.
//
// Semantics (specs/pipe.tla for the wait/wake, I-9):
//   - read drains 1..n bytes, blocks while the ring is empty and the write
//     end open, and returns 0 at EOF.
//   - write of n <= PIPE_BUF_SIZE proceeds only when all n fit (POSIX
//     PIPE_BUF atomicity); a larger write fills what room there is. It
//     blocks while it cannot proceed and the read end is open, and returns
//     -T_E_PIPE, posting the `pipe` note, once the read end is closed.
//   - Any number of readers and writers may block on either end.
//   - A CNONBLOCK end returns -T_E_AGAIN where it would block; a CNBFRAME
//     end (the 9P transport's tx) writes whole frames or nothing.
//   - A blocked wait returns -1 when the Proc is dying, and -T_E_INTR when
//     a caught note ends a Linux caller's wait (ARCH 8.8.3), having moved
//     nothing.
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
