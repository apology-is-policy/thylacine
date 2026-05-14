# 58. Corvus hardening syscalls (P5-corvus-syscalls)

Five v1.0 hardening syscalls + two new capabilities + per-Proc flags. The foundational scaffold consumed by corvus + per-user stratumd at startup per `CORVUS-DESIGN.md §4.1.1`. Each syscall either sets a one-way per-Proc flag (PROC_FLAG_*) or performs a one-shot action (`explicit_bzero`, `getrandom`).

This chunk is forward-compat scaffolding — at v1.0 several of the flags don't have enforcing subsystems yet (no swap, no core dumps, no debug Spoors). The syscalls exist so corvus can call them at startup; the flags will be honored by the relevant subsystems when those subsystems land.

---

## Purpose

CORVUS-DESIGN §4.1.1 enumerates the runtime hardening corvus requires:
- pages mlock'd (no swap leak)
- core dumps disabled (no RAM-to-disk via crash dump)
- no debug-Spoor attach (no debugger memory peek)
- secret-wipe primitive (no compiler-elided memset)
- CSPRNG-seeded probe (refuse to generate randomness until ready)

This chunk lands the syscall surface for all five.

---

## ABI

### SYS_MLOCKALL

```
SYS_MLOCKALL = 16

Args:
  x0 = flags (u32; reserved — pass 0 at v1.0; v1.x adds MCL_CURRENT/MCL_FUTURE)

Return:
  x0 = 0 on success
  x0 = -1 on:
    - caller lacks CAP_LOCK_PAGES
```

On success, sets `PROC_FLAG_MLOCKED` on the calling Proc. Idempotent (a second call returns 0 without state change). v1.0 has no swap, so the flag is forward-compat scaffolding; corvus calls it at startup to satisfy invariant C-2.

### SYS_SET_DUMPABLE

```
SYS_SET_DUMPABLE = 17

Args:
  x0 = dumpable (u32; 0 = disable, 1 = enable)

Return:
  x0 = 0 on success
  x0 = -1 on:
    - dumpable not in {0, 1}
    - dumpable == 1 AND PROC_FLAG_NODUMP already set (one-way)
```

One-way: `SYS_SET_DUMPABLE(0)` sets `PROC_FLAG_NODUMP`. A subsequent `SYS_SET_DUMPABLE(1)` is **refused** to prevent attackers escalating-from-elevated to "now please let me crash-dump." Idempotent for `SYS_SET_DUMPABLE(0)`.

### SYS_SET_TRACEABLE

```
SYS_SET_TRACEABLE = 18

Args:
  x0 = traceable (u32; 0 = disable, 1 = enable)

Return:
  x0 = 0 on success
  x0 = -1 on:
    - traceable not in {0, 1}
    - traceable == 1 AND PROC_FLAG_NOTRACE already set (one-way)
```

Same shape as `SYS_SET_DUMPABLE`. v1.0 has no debug Spoors; the flag enforces when they land (CORVUS-DESIGN §11 v1.x).

### SYS_EXPLICIT_BZERO

```
SYS_EXPLICIT_BZERO = 19

Args:
  x0 = buf_va (user-VA pointer)
  x1 = len (bytes; ≤ SYS_RW_MAX = 4096 per call)

Return:
  x0 = 0 on success
  x0 = -1 on:
    - buf_va outside user-VA bound (UACCESS_USER_VA_TOP = 1 << 47)
    - buf_va + len overflows or exceeds the bound
    - any uaccess_store_u8 fails (e.g., write to a read-only mapping)
```

Per-byte zeroize via `uaccess_store_u8`. The kernel/user boundary is the compiler barrier — the optimizer cannot prove the stores are dead across the syscall edge. For larger buffers, userspace loops.

### SYS_GETRANDOM

```
SYS_GETRANDOM = 20

Args:
  x0 = buf_va (user-VA destination)
  x1 = len (bytes; ≤ SYS_RW_MAX = 4096 per call)
  x2 = flags (u32; GRND_NONBLOCK = 1 — non-blocking)

Return:
  x0 = bytes_read (= len on full success)
  x0 = -1 on:
    - caller lacks CAP_CSPRNG_READ
    - buf_va bound violation
    - kernel CSPRNG not seeded (returns -1 immediately at v1.0 — no
      software-CSPRNG mixing means RNDR-absent is permanent)
    - CSPRNG hardware fault (partial RNDR fill)
```

Backed by `kern_random_bytes` (ARM FEAT_RNG / RNDR instruction). 4 KiB kernel-stack scratch + per-byte `uaccess_store_u8` (mirrors SYS_READ's bounce pattern).

---

## Capabilities

| Cap | Holders | Required for |
|---|---|---|
| `CAP_LOCK_PAGES` (1 << 1) | kproc, corvus, per-user stratumd | `SYS_MLOCKALL` |
| `CAP_CSPRNG_READ` (1 << 2) | kproc, corvus, most user procs (broadly granted) | `SYS_GETRANDOM` |

`CAP_HW_CREATE` (1 << 0) unchanged.

`CAP_ALL = CAP_HW_CREATE | CAP_LOCK_PAGES | CAP_CSPRNG_READ` (kproc's mask).

Future `CAP_HOSTOWNER` is reserved but not defined at this chunk — lands at P5-hostowner.

---

## Per-Proc flags

```c
struct Proc {
    ...
    u32 proc_flags;
    u32 _pad_flags;
};

#define PROC_FLAG_NODUMP    (1u << 0)
#define PROC_FLAG_NOTRACE   (1u << 1)
#define PROC_FLAG_MLOCKED   (1u << 2)
```

Zero-initialized at `proc_alloc` (default = dumpable, traceable, not-mlocked). One-way bits — set via syscall, never cleared.

struct Proc grew from 128 to 136 bytes; the `_Static_assert` in `proc.h` was updated to pin the new size. SLUB cache for Proc accommodates the larger object without other changes.

---

## Userspace API — `<thyla/syscall.h>`

```c
long t_mlockall(unsigned long flags);
long t_set_dumpable(unsigned long dumpable);
long t_set_traceable(unsigned long traceable);
long t_explicit_bzero(void *buf, size_t len);
long t_getrandom(void *buf, size_t len, unsigned long flags);

#define T_GRND_NONBLOCK 1u
```

Five inline-asm stubs in `usr/lib/libt/include/thyla/syscall.h` matching the kernel ABI.

---

## Implementation

`kernel/syscall.c` adds 5 SVC handlers + 3 non-static `sys_*_for_proc` inners (for kernel-internal tests; the `explicit_bzero` and `getrandom` handlers are tested via their underlying primitives + future userspace probes).

`kernel/random.c` extracts a public `kern_random_bytes(buf, len)` + `kern_random_seeded()` from the previously-static devrandom logic; `devrandom_read` is now a thin wrapper.

`kernel/include/thylacine/random.h` (new) declares the public CSPRNG API.

---

## Tests

`kernel/test/test_sys_corvus.c` — 6 tests:

- `sys_mlockall.cap_gate` — refused without `CAP_LOCK_PAGES`; succeeds with cap; sets `PROC_FLAG_MLOCKED`; idempotent.
- `sys_set_dumpable.one_way_to_zero` — defaults are dumpable; `(0)` sets `NODUMP`; `(1)` after `(0)` is refused; bad args rejected.
- `sys_set_traceable.one_way_to_zero` — same shape, for `NOTRACE`.
- `sys_corvus_caps.kproc_has_new_caps` — kproc carries `CAP_LOCK_PAGES | CAP_CSPRNG_READ | CAP_HW_CREATE` via `CAP_ALL`.
- `kern_random.seeded_returns_true_on_qemu` — `kern_random_seeded()` returns true on QEMU virt (FEAT_RNG present).
- `kern_random.bytes_produces_nonzero` — 32-byte reads contain at least one non-zero byte; two reads differ; zero-length is no-op; negative-length / NULL rejected.

SYS_EXPLICIT_BZERO and SYS_GETRANDOM SVC paths are exercised by their underlying primitives + future userspace probes at P5-corvus-bringup (when corvus binary actually calls them).

Tests landed: 410 → 416 PASS × default + UBSan.

---

## Composition with future chunks

- **P5-corvus-bringup**: corvus's startup code calls `t_mlockall(0)`, `t_set_dumpable(0)`, `t_set_traceable(0)`, and probes `t_getrandom(buf, 8, T_GRND_NONBLOCK)` to verify CSPRNG ready. All five syscalls are exercised end-to-end at EL0.
- **P5-hostowner**: introduces `CAP_HOSTOWNER`. The `CAP_ALL` macro is extended again; the static_assert in caps.h catches drift.
- **v1.x swap subsystem**: when swap lands, the swap-out path checks `proc_flags & PROC_FLAG_MLOCKED` and skips. The flag has been honored from this chunk onward.
- **v1.x debug Spoor surface**: when debug attach lands, the attach gate checks `proc_flags & PROC_FLAG_NOTRACE` and refuses.

---

## Performance characteristics

All five syscalls are O(1) except `SYS_EXPLICIT_BZERO` and `SYS_GETRANDOM`, which are O(N) in `len` (bounded by `SYS_RW_MAX = 4096`). Per-call cost dominated by:
- SVC entry/exit (~50 cycles).
- For bzero/getrandom: 4096 × uaccess_store_u8 (~few hundred cycles each iteration due to fault-fixup machinery, but typically no faults on hot path).

Not on any latency-sensitive critical path. corvus's startup runs all five once.

---

## Status

| Item | State |
|---|---|
| SYS_MLOCKALL handler + libt stub | LANDED |
| SYS_SET_DUMPABLE handler + libt stub | LANDED |
| SYS_SET_TRACEABLE handler + libt stub | LANDED |
| SYS_EXPLICIT_BZERO handler + libt stub | LANDED |
| SYS_GETRANDOM handler + libt stub | LANDED |
| CAP_LOCK_PAGES + CAP_CSPRNG_READ | LANDED |
| PROC_FLAG_NODUMP/NOTRACE/MLOCKED | LANDED |
| `kern_random_bytes` / `kern_random_seeded` public surface | LANDED |
| Tests | 6 tests, LANDED |
| Enforcement of flags by future subsystems | DEFERRED (swap, core dumps, debug attach all v1.x) |

---

## Known caveats

1. **Flags are not enforced at v1.0.** PROC_FLAG_NODUMP / NOTRACE / MLOCKED don't gate anything at v1.0 because the enforcing subsystems (swap, core dumps, debug Spoors) don't exist yet. corvus calls the syscalls anyway so its startup is forward-compatible.
2. **`SYS_SET_DUMPABLE(1)` and `SYS_SET_TRACEABLE(1)` on a Proc that hasn't yet set the corresponding NO-flag return 0** — they're no-ops (already in the allowed state). This is intentional; userspace can probe the current state by attempting to set 1 (returns -1 if disabled).
3. **`SYS_GETRANDOM` is non-blocking at v1.0** regardless of `GRND_NONBLOCK`. Software-CSPRNG mixing (v1.x) introduces a real seeding state machine; until then, the only failure mode is FEAT_RNG absent (which means the CSPRNG never becomes ready).
4. **No syscall for clearing the flags.** This is deliberate — the security model is "Proc declares stronger constraints monotonically." A future hardened-deployment Proc that wants stronger flags can't be coerced into weakening them.
