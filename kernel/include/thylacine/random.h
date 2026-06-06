// Kernel CSPRNG public API (P5-corvus-syscalls).
//
// Per CORVUS-DESIGN.md §4.1.1 + C-15. SYS_GETRANDOM consumes this
// surface. The implementation lives in kernel/random.c: a ChaCha20
// forward-secure CSPRNG (the arc4random construction) seeded from a
// mixed entropy pool (DTB boot seed + CNTPCT jitter + RNDR-when-present
// + a kernel virtio-rng pull). The Plan 9 /dev/random Dev surface is a
// thin wrapper over this API. Lazarus W3 (PORTABILITY.md §6) replaced
// the RNDR-only baseline so the same path runs on RNDR-less targets.
//
// Invariants:
//   - kern_random_seeded() is the readiness signal: false until a
//     strong entropy source has ever contributed (DTB boot seed, RNDR,
//     or a virtio-rng pull), monotonic true thereafter. While false,
//     kern_random_bytes returns -1 (fail closed) and SYS_GETRANDOM
//     refuses -- the same contract the RNDR-only baseline held.
//   - The CSPRNG re-keys on every keystream-buffer drain (fast key
//     erasure -> backtracking resistance) and pulls fresh entropy every
//     ~1 MiB served (state-compromise recovery).

#ifndef THYLACINE_RANDOM_H
#define THYLACINE_RANDOM_H

#include <thylacine/types.h>

// Read `len` random bytes from the kernel CSPRNG into `out`.
//
// Returns:
//   len on full success (every requested byte produced).
//   0   if len == 0 (no-op).
//   -1  if the CSPRNG is unavailable OR retries exhausted mid-stream
//       (caller may retry the partial-tail case; v1.0 returns -1 for
//       both since the user-visible distinction would require returning
//       partial counts and the syscall layer prefers atomic results).
//
// Pre-condition: `out` must reference at least `len` bytes of kernel-
// addressable memory. Callers from syscall context use a bounce
// buffer + uaccess_store_u8 per byte for user-VA targets.
long kern_random_bytes(void *out, long len);

// Is the kernel CSPRNG ready to deliver bytes? Returns true iff a strong
// entropy source has ever seeded the pool (DTB boot seed, RNDR, or a
// virtio-rng pull); monotonic. kern_random_bytes returns -1 while false.
bool kern_random_seeded(void);

// Pull strong entropy from the kernel virtio-rng device and re-key the
// CSPRNG. Called once at boot (after virtio_init, from kernel/main.c) and
// on the reseed cadence. Returns the number of virtio-rng bytes obtained
// (0 if no RNG device is present -- the CSPRNG keeps its prior seed).
size_t random_seed_from_virtio(void);

// True iff a virtio-rng pull has ever contributed entropy to the pool.
bool kern_random_virtio_contributed(void);

#endif // THYLACINE_RANDOM_H
