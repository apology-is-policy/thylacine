// vdso.h — the clock vDSO ABI: the read-only timekeeping page userspace reads
// instead of SYS_CLOCK_GETTIME. Binding design: docs/VDSO-DESIGN.md.
#ifndef THYLACINE_VDSO_H
#define THYLACINE_VDSO_H

#include <thylacine/types.h>

#define VDSO_CLOCK_MAGIC   0x5644534f4c4b3031ull  // "VDSOLK01" — validity + version sentinel
#define VDSO_CLOCK_VERSION 1u

// One kernel-owned page, mapped READ-ONLY (RO+XN) into every Proc at the user VA
// delivered in the AT_VDSO_CLOCK auxv entry. The SAME physical page for all Procs
// (the shared-vvar model — there is no per-Proc timekeeping state). Userspace
// reads CNTVCT_EL0 directly (already EL0-enabled, CNTKCTL_EL1.EL0VCTEN) + this
// page to compute CLOCK_MONOTONIC / CLOCK_REALTIME WITHOUT a syscall:
//     mono = (cnt / freq) * 1e9 + (cnt % freq) * 1e9 / freq   // == timer_now_ns()
//     real = mono + wall_offset_ns
// No seqlock: freq is write-once before smp_init; wall_offset_ns is a single
// aligned u64 (one atomic store from SYS_CLOCK_SETTIME) -> a reader sees
// old-or-new, never torn. A reader whose magic/version does not validate MUST
// fall back to SYS_CLOCK_GETTIME. ABI is append-only: bump VERSION + consume the
// reserved tail, never move a field's offset.
struct vdso_clock {
    u64 magic;            // == VDSO_CLOCK_MAGIC
    u64 version;          // == VDSO_CLOCK_VERSION (append-only)
    u64 freq;             // CNTVCT frequency in Hz (write-once == g_freq)
    u64 wall_offset_ns;   // CLOCK_REALTIME = mono + this (atomic; settime updates it)
    u64 reserved[4];      // zeroed; future seqlock / coarse-time / clock-source id
};

_Static_assert(sizeof(struct vdso_clock) == 64,
               "vdso_clock ABI: 64-byte page header");
_Static_assert(__builtin_offsetof(struct vdso_clock, magic) == 0,
               "vdso_clock.magic @0");
_Static_assert(__builtin_offsetof(struct vdso_clock, version) == 8,
               "vdso_clock.version @8");
_Static_assert(__builtin_offsetof(struct vdso_clock, freq) == 16,
               "vdso_clock.freq @16");
_Static_assert(__builtin_offsetof(struct vdso_clock, wall_offset_ns) == 24,
               "vdso_clock.wall_offset_ns @24");

#endif // THYLACINE_VDSO_H
