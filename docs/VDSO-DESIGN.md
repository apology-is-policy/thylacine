# VDSO-DESIGN.md — the monotonic-clock vDSO

Binding design for the Thylacine clock vDSO: a kernel-provided read-only
timekeeping page that lets userspace compute `CLOCK_MONOTONIC` /
`CLOCK_REALTIME` by reading `CNTVCT_EL0` plus the page, **without a syscall**.

Status: **scripture (sub-chunk 1)** — this document + the ABI
(`kernel/include/thylacine/vdso.h` + the `AT_VDSO_CLOCK` auxv tag in `elf.h`).
No functional code yet; the kernel page + the userspace readers land in the
subsequent sub-chunks. Realizes the v1.x vDSO already anticipated in
`ARCHITECTURE.md §22.6` (the §4.5 per-syscall-cost backlog, #62).

User-chosen 2026-06-25 (signed off) as the fix for the #343 second issue.

---

## 1. Why

`#343` began as "the REVENANT file-backed-exec page-in is slow"; measurement
(five instrumented boots, `memory/project_go_arc.md`) **refuted** that — the
on-device `go build`'s wall time is **not** REVENANT page-in and **not** FS
reads (both the demand-page fault counters and `dev9p_read` stayed frozen
through the entire compile). A per-syscall histogram found the cause: the Go
runtime's scheduler (`findRunnable` / `sysmon`) hammers **`nanotime()`**, which
on Thylacine is a full `SYS_CLOCK_GETTIME` syscall — **740 million calls** for a
single `go tool compile` of a two-line file, plus ~50M futex park/unpark.

On Linux `nanotime()` is a **vDSO read** (~20 ns, no trap); on Thylacine it is a
trap (~0.5 µs+ each). Go's *normal* idle-scheduler behaviour is therefore
catastrophically syscall-bound. (A go-fork futex bug — `futexsleep` passing the
kernel's "return-at-once" timeout `0` for Go's "sleep forever" — was the *first*
#343 fix; it cut a 982M-call torpor_wait spin but exposed this clock-syscall
floor underneath.)

The vDSO removes `nanotime()` from the syscall path. It benefits **all**
syscall-heavy native userspace (the scheduler-bound Go toolchain most acutely,
but also any libthyla-rs program that reads the clock in a hot loop), not just
Go.

## 2. The key simplification — a `vvar` data page, not a code vDSO

Linux's vDSO is two pieces: a **code** page (`__kernel_clock_gettime` + the ELF
the loader resolves symbols against) and a **`vvar`** data page (the seqlocked
timekeeping inputs the code reads). Thylacine needs **only the data half**,
because:

- **EL0 already reads `CNTVCT_EL0` directly.** `timer_enable_el0_counter_access`
  sets `CNTKCTL_EL1.EL0VCTEN` (+ `EL0PCTEN`) at boot, per-CPU
  (`arch/arm64/timer.c`). So userspace can read the architectural counter itself
  — no kernel *code* needs to run in EL0, hence no code page and no ELF symbol
  resolution. The reader does the `cnt → ns` arithmetic inline.

- **No seqlock is needed.** The reader needs exactly two values, and both are
  already trivially consistent:
  - **`freq`** (the `CNTVCT` frequency in Hz, == the kernel's `g_freq`): written
    **once** before `smp_init` and never again. Stable for all time. (It *must*
    come from the page: `CNTFRQ_EL0` is **not** EL0-readable — only the counter
    is — so userspace cannot read the frequency from the hardware.)
  - **`wall_offset_ns`** (`CLOCK_REALTIME = mono + offset`, == the kernel's
    `g_wallclock_offset_ns`): a **single aligned `u64`**, written only by
    `SYS_CLOCK_SETTIME → timer_reset_wallclock_anchor_ns` via one
    `__atomic_store_n` (the LS-K / net-7a single-`u64` design, `ARCH §22.6`). A
    concurrent reader sees old-or-new, **never torn** — so a plain relaxed
    atomic load on the page suffices. No seqlock, no retry loop, no generation
    counter.

The reader replicates `timer_now_ns()` verbatim (`arch/arm64/timer.c`):

```
cnt  = read CNTVCT_EL0                     // EL0 MRS, no trap
mono = (cnt / freq) * 1e9 + (cnt % freq) * 1e9 / freq   // split form: no u64 overflow
real = mono + wall_offset_ns
```

The split (quotient, remainder) form is the kernel's own — it avoids the `cnt *
1e9` overflow (~5 min at 62.5 MHz). The reader uses it identically, so the vDSO
value is **bit-identical** to what `SYS_CLOCK_GETTIME` would have returned at the
same `cnt`.

## 3. The page

One kernel-owned page, the **same physical page mapped read-only into every
Proc** (the Linux shared-`vvar` model — there is no per-Proc timekeeping state).

```c
// kernel/include/thylacine/vdso.h  (ABI-bearing; _Static_assert-pinned)
#define VDSO_CLOCK_MAGIC   0x5644534f4c4b3031ull  // "VDSOLK01" — validity + version sentinel
#define VDSO_CLOCK_VERSION 1u

struct vdso_clock {
    u64 magic;            // == VDSO_CLOCK_MAGIC; a reader that mismatches falls back to the syscall
    u64 version;          // layout version (append-only; a higher version a reader does not know -> fallback)
    u64 freq;             // CNTVCT Hz (== g_freq; write-once before smp_init)
    u64 wall_offset_ns;   // CLOCK_REALTIME = mono + this (atomic; SYS_CLOCK_SETTIME updates it)
    u64 reserved[4];      // future: a seqlock / coarse-time / clock-source id, WITHOUT a layout bump
};
_Static_assert(sizeof(struct vdso_clock) == 64, "vdso_clock ABI size");
// + offset asserts on magic/version/freq/wall_offset_ns
```

The page is `PROT_READ` only to EL0 (RO + XN). It holds **only** these
timekeeping scalars — no kernel pointers, no KASLR base, no secrets — so it is a
negligible information-exposure surface (the frequency and a wall-clock offset
are public facts). It is a **dedicated** page, never piggybacked on a page that
also holds kernel state.

## 4. Discovery — auxv

`exec.c` already builds a System V aux vector (`AT_PHDR` / `AT_PHENT` /
`AT_PHNUM` / `AT_PAGESZ` / `AT_RANDOM` / `AT_NULL`; `elf.h`). The vDSO page VA is
delivered as a new entry:

```c
#define AT_VDSO_CLOCK 0x5654   // Thylacine-private auxv tag (a_val = user VA of the vdso_clock page).
                               // Deliberately OUTSIDE the Linux/System V AT_ range so a pouch/musl
                               // binary stores-and-ignores it (musl queries only the standard tags;
                               // it must NEVER be confused with AT_SYSINFO_EHDR=33, which musl would
                               // try to parse as a vDSO ELF). Native readers (libthyla-rs, the Go
                               // fork) query it explicitly.
```

The reader captures `AT_VDSO_CLOCK` **once** at `_start` (the runtime already
walks auxv for `AT_RANDOM`), validates `magic` + `version`, and caches the page
pointer. **Zero runtime syscalls.** A `SYS_VDSO_BASE` discovery syscall was the
alternative and was rejected — auxv is the idiom, costs no trap, and reuses the
startup walk the runtime already does.

## 5. The seams

### Kernel
- A `struct vdso_clock *g_vdso_page` — allocate one page at boot **after**
  `timer_init` (so `freq` is known) and the boot wall-anchor; populate
  `magic`/`version`/`freq`/`wall_offset_ns`.
- `vdso_clock_publish_wall(off)` — called from `timer_reset_wallclock_anchor_ns`
  (the `SYS_CLOCK_SETTIME` path) so the page's `wall_offset_ns` tracks
  `g_wallclock_offset_ns` (one extra atomic store; keep both in sync — or make
  the page the single source `timer_realtime_ns` also reads).
- `exec_setup` (+ the auxv builder): map the **one shared page** RO+XN into the
  new Proc at a kernel-chosen user VA, and push `AT_VDSO_CLOCK = <va>`. Mapping
  mechanism: a RO `burrow_map` of a kernel-owned Burrow wrapping the page (a thin
  `burrow_create_kernel_ro(page)` if no existing kernel-page-RO path fits).

### Go fork (`~/projects/go-thylacine`)
- `src/runtime/os_thylacine.go`: at the auxv parse (`sysargs`), capture
  `AT_VDSO_CLOCK` into a runtime global `vdsoClockBase`.
- `nanotime1()` / `walltime()` (today pure syscalls): if `vdsoClockBase != 0` and
  the page's magic/version validate, read `CNTVCT_EL0` (a new
  `runtime·read_cntvct` = `MRS Rx, CNTVCT_EL0` in `sys_thylacine_arm64.s`) +
  `freq` / `wall_offset_ns` and compute; **else fall back** to the
  `clock_gettime` syscall (kept for the fallback and for `settime`).

### libthyla-rs (`usr/lib/libthyla-rs`)
- The native `time` / `Instant` / `SystemTime` (today `clock_gettime`-backed, the
  LS-K row) read the page found via auxv at `_start`. Same fast-path + fallback.

## 6. Invariants & audit posture

A new EL0-facing kernel page + a new ABI ⇒ **audit-bearing** (`ARCH §25.4`),
prose-validated (no new TLA+; the atomicity argument is a single-`u64` load, the
established LS-K/net-7a pattern).

- **I-12 (W^X):** the page is RO + XN data — clean; a `_Static_assert` + a PTE-prot
  review confirm no W and no X.
- **I-13 (kernel/user isolation):** a kernel page mapped RO to EL0 — but it holds
  **only** `{magic, version, freq, wall_offset_ns}` (+ zeroed reserved), no
  pointers/secrets/KASLR. Prosecute: it is a *dedicated* page (nothing else lives
  on it); a user **write faults** (RO) and can never corrupt the shared timebase
  for other Procs.
- **I-15 (hardware view from DTB):** `freq` derives from `CNTFRQ` at `timer_init`,
  DTB-consistent.
- **Atomicity (the no-seqlock claim):** prosecute the cross-Proc read-vs-settime
  race — every Proc reads the one shared page while a `CAP_HOSTOWNER`
  `SYS_CLOCK_SETTIME` updates `wall_offset_ns`; the single aligned `u64` store
  gives old-or-new, never torn; `CLOCK_MONOTONIC` never goes backward (freq
  stable, `CNTVCT` monotonic).
- **ABI:** the page layout + the `AT_VDSO_CLOCK` tag are ABI — `magic` + `version`
  + `_Static_assert`s; **append-only**. A reader that sees a bad magic / an
  unknown-higher version **falls back to the syscall** — a kernel without the
  vDSO, or a future layout bump, never breaks userspace.
- **Fallback correctness (load-bearing):** every reader (Go + libthyla-rs) MUST
  fall back to `SYS_CLOCK_GETTIME` if the page is absent / magic mismatches. The
  syscall path is retained, not deleted.

## 7. Sub-chunks (design-first)

1. **scripture (this)** — this doc + `ARCH §22.6` update + the ABI
   (`vdso.h` `struct vdso_clock` + `AT_VDSO_CLOCK` in `elf.h`). No code.
2. **kernel** — the page (alloc/populate/settime-sync) + the `exec_setup` RO map
   + the auxv entry + the `ARCH §25.4` audit-trigger row + kernel tests (the page
   maps RO; a user write faults; the values equal `SYS_CLOCK_GETTIME` at the same
   instant).
3. **Go fork + re-measure** — auxv capture + `nanotime1`/`walltime` fast-path +
   the `CNTVCT` asm + the fallback. Re-run the instrumented boot (the syscall
   histogram is still in place): `SYS_CLOCK_GETTIME` should crash from 740M to
   ~thousands and the compile should get **much** faster.
4. **libthyla-rs** — the native reader + a bench delta (cpubench / netperf
   timing).
5. **audit** — the focused EL0-page / atomicity / fallback audit + the SMP gate.

**Measured target:** the `go tool compile` of a two-line file drops from ~300 s+
toward seconds. The residual after the vDSO is the ~50M futex park/unpark churn
(each torpor op ~36 µs) — addressed *separately* by a faster torpor or
GOMAXPROCS tuning, out of scope here.
