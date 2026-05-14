// Kernel CSPRNG public API (P5-corvus-syscalls).
//
// Per CORVUS-DESIGN.md §4.1.1 + C-15. SYS_GETRANDOM consumes this
// surface. The implementation lives in kernel/random.c and uses the
// ARM RNDR instruction (FEAT_RNG) on v1.0. Pre-existing devrandom_*
// surface (Plan 9 /dev/random style) continues to work and is now
// implemented as a thin wrapper over this API.
//
// v1.0 invariants:
//   - If RNDR is available (g_rndr_available), reads are always-
//     seeded — the hardware CSPRNG is its own entropy source.
//   - kern_random_seeded() returns the readiness signal — false iff
//     hardware support is absent. v1.x with software CSPRNG mixing
//     extends to a real seeding state machine.

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

// Is the kernel CSPRNG ready to deliver bytes? Returns true iff
// kern_random_bytes is expected to succeed on a typical call.
// v1.0: true iff ARM FEAT_RNG is present (no software fallback).
bool kern_random_seeded(void);

#endif // THYLACINE_RANDOM_H
