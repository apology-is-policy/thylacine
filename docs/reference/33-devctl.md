# 33 — devctl: kernel admin Dev (P4-D)

The kernel's introspection-and-administration Dev. Plan 9 idiom: synthetic /ctl/ namespace exposing live kernel state as text-format files. Reads return current values; writes (admin commands) are deferred to Phase 5+ when the syscall surface lands. Per ARCH §9.4 + ROADMAP §6.1.

dc='C' (uppercase to leave 'c' for cons).

---

## Purpose

Gives operators (and the future userspace shell) a uniform way to query kernel state without bespoke syscalls. Plan 9 chose this design because (a) reading text from a synthetic file is composable with `cat`/`grep`/`awk` shell idiom, and (b) the same protocol that ships across 9P serves both local introspection and remote administration of mounted Thylacine instances.

The five v1.0 leaves cover the kernel state most often inspected:

- `/ctl/procs` — process listing (PID/state/threads).
- `/ctl/memory` — physical memory stats.
- `/ctl/devices` — bestiary listing (registered Devs).
- `/ctl/kernel-base` — KASLR base + offset + entropy source.
- `/ctl/sched` — runnable count.

Future Phase 4+ chunks add the deeper namespace per ARCH §9.4 (`/ctl/kernel/`, `/ctl/sched/`, `/ctl/9p/`, `/ctl/irq/`, `/ctl/mm/`, `/ctl/security/`, `/ctl/log/`) once nested-directory walks are needed by enough callers to justify the qid-encoding extension.

---

## Public API — `<thylacine/dev.h>`

```c
extern struct Dev devctl;        // dc='C', name="ctl"
```

The Dev's vtable follows the directory-Dev pattern from devproc (`30-dev-spoor.md` is the substrate; `32-devproc.md` is the prior peer). Walks resolve leaf names to qids; reads dispatch by qid kind into per-leaf format generators; writes return -1 (admin commands deferred).

---

## Namespace + qid encoding

```
/ctl                              path = 0                      QTDIR
/ctl/procs                        path = CTL_KIND_PROCS         QTFILE
/ctl/memory                       path = CTL_KIND_MEMORY        QTFILE
/ctl/devices                      path = CTL_KIND_DEVICES       QTFILE
/ctl/kernel-base                  path = CTL_KIND_KERNEL_BASE   QTFILE
/ctl/sched                        path = CTL_KIND_SCHED         QTFILE
/ctl/cpu                          path = CTL_KIND_CPU           QTFILE
```

Single-level layout — no per-pid axis (unlike devproc). Subkind enum values 1..6 are leaf kinds; 0 is the root sentinel. New leaves get the next sequential subkind; reserved range up to 31 (single byte) before nested directories need richer encoding.

### Namespace residence (#57a)

devctl is **mounted at `/ctl`** in the boot namespace (`kernel/joey.c::joey_mount_static_dev`, the /srv idiom), grafted onto a synthetic devramfs `/ctl` mount-point dir and re-grafted onto the pivoted disk root by the long-running init. Before #57a devctl was kernel-internal — unreachable by a path.

`/ctl` is **read-only** (`devctl_write` returns -1) and `devctl.perm_enforced` is unset (false), so it is world-readable introspection — **with one exception**: `/ctl/kernel-base` discloses the live KASLR slide (`kernel_base` + `kaslr_offset`), an I-16 secret, so that ONE leaf is gated on **`CAP_HOSTOWNER`** at the `devctl_read` site (`devctl_kernel_base_readable(caller)`; the #57a focused-audit F1 fix). `CAP_HOSTOWNER` is elevation-only (not even kproc holds it by default — only corvus's ADMIN_ELEVATE grants it), so the slide is **admin-only-via-elevation**; an unprivileged user or container cannot read it and defeat KASLR. The coarse procs/memory/devices/sched leaves stay world-readable.

The #57a focused audit also closed two pre-existing read-path latents the mount activates: a cross-Proc **UAF** (`devproc_read`/`devproc_stat_native` dereferenced a `Proc` after the proc-table lock was released — see `docs/reference/32-devproc.md`; the same fix applies to devctl's `proc_for_each` consumers) and the unbounded `/ctl/procs` IRQ-off proc-table walk (`format_procs_cb` now stops once the read buffer fills, bounding an EL0 tight-loop's lock-hold; the residual O(N) global-lock walk is the #62 scalability seam).

Reaching `/ctl` through `stalk` required the same **reuse-`nc` walk fix** as devproc (see `docs/reference/32-devproc.md`, "Namespace residence"): `devctl_walk` now returns the caller's pre-clone `nc` as `wq->spoor` (0-element walk → `nqid == 0`), the shape `clone_walk_zero`'s mount-cross needs, with the `nc == NULL` legacy path preserved for the direct-call kernel tests.

---

## File contents (v1.0)

### `/ctl/procs`

The system-wide process overview. **prowl-1 (PROWL-DESIGN.md §3) added the `NAME` + `CPU_NS` columns** (the #65 `PAGES`/`CHILDREN` columns predate it); **prowl-4 added the `PPID` column** (the tree view) **and the `STOPPED` run-state**:

```
PID    PPID    NAME    STATE    THREADS    PAGES    CHILDREN    CPU_NS
0    0    kproc    ALIVE    1    0    3    12345678
```

Columns are whitespace-separated (a monitor splits on whitespace; process names are basenames, never contain spaces; an unstamped Proc reads `?`). `PPID` is the parent pid (`p->parent ? p->parent->pid : 0` — read under `g_proc_table_lock`, so `p->parent` is a valid Proc or NULL; kproc + a reparented orphan-root read 0), the edge a monitor builds a parent→child tree from. `CPU_NS` is the cumulative on-CPU time in ns (`proc_cpu_ns` = Σ `run_ns` over the Proc's threads) — the reader diffs it across polls for %CPU. **`STATE` is the effective run-state (prowl-4):** an ALIVE Proc with `job_stop_req` set (a monitor `suspend` via `/proc/<pid>/ctl`, or a Ctrl-Z via the pts path) reads **`STOPPED`** (the Unix `ps` T-state, via `procs_state_name` — a plain `state_name` otherwise); the DEBUG stop (`debug_stop_req`, the attach-gated debugger stop) is deliberately **not** surfaced here — it is the debugger's private I-39 view, not a job-control state a monitor should expose. `job_stop_req` is read atomically (a cross-Proc reader holds `g_proc_table_lock` via `proc_for_each` but no per-Proc lock). Walks via `proc_for_each(callback, arg)` (declared in `<thylacine/proc.h>`); the callback formats one row per Proc into the buffer, and `proc_cpu_ns` walks `p->threads` under the same `g_proc_table_lock` the DFS holds. `format_procs_cb` early-returns on buffer overflow (the #57a bound); prowl-1 bumped `DEVCTL_READ_BUF` 512 → 2048 so the wider lines do not truncate the listing before ~30 procs. Total cost is O(N_procs) with small per-proc text.

### `/ctl/memory`

```
total:    8192 pages
free:     7521 pages
reserved: 671 pages
```

Direct calls to `phys_total_pages` / `phys_free_pages` / `phys_reserved_pages`. Each call is O(1). Pages, not bytes — page size is 4 KiB at v1.0 (`PAGE_SIZE`).

### `/ctl/devices`

```
DC  NAME
-   none
c   cons
0   null
z   zero
r   random
p   proc
C   ctl
```

Iterates `bestiary[]` (sentinel-terminated) — the Dev registry from P4-A. dc + name per row.

### `/ctl/kernel-base`

```
kernel_base:  0xffff800000XXXXXX
kaslr_offset: 0xXXXXXX
seed_source:  dtb-kaslr-seed
```

KASLR diagnostics. seed_source is one of `dtb-kaslr-seed` / `dtb-rng-seed` / `cntpct-fallback` per `kaslr_seed_source_str(kaslr_get_seed_source())`. Hex format uses 0x-prefixed lowercase via `fmt_uhex`.

### `/ctl/sched`

```
runnable: 1
```

Calls `sched_runnable_count()`. Per-band breakdown via `sched_runnable_count_band(band)` is held to a future sub-chunk that adds the band-specific output. (`/ctl/sched` also renders `cpus:` + the global work-conservation `wc:` / `wc-tickless:` lines — see `format_sched`.)

### `/ctl/cpu` (prowl-3b)

One row per online CPU — the per-core meter denominator `prowl` reads:

```
cpus: 4
cpu idle_ns capacity
0 42441000000 1024
1 38102773311 1024
2 40917552108 1024
3 39558210447 1024
```

`idle_ns` is the cumulative ns that CPU spent parked in the idle loop
(`sched_cpu_idle_ns(cpu)`, charged in `sched_idle_park` next to the global
`g_wc_idle_ns` — prowl-3a); `capacity` is the normalized capacity class
(`sched_cpu_capacity`, `SCHED_CAPACITY_SCALE`=1024 on a uniform topology). The
reader derives **per-core utilization** the htop way: `1 - d(idle_ns)/d(wall)`
diffed across two polls (the kernel keeps no instantaneous-rate state). Bounded
by `smp_cpu_count()` (`<= DTB_MAX_CPUS`=8 rows); the accessors self-guard an
out-of-range index. A DTB-declared CPU that never came online (a PSCI bring-up
failure — `idle_ns` stuck at 0, indistinguishable from a pegged-100%-busy core
via idle_ns alone) renders as `<i> offline` (gated on `g_cpu_online[i]`; the
reader's 3-column parse skips the 2-column line → no bar), never a spurious full
meter (prowl-5 F2; unreachable on QEMU-virt, which never fails PSCI). **All-visible** like the other coarse `/ctl` leaves
(visibility-not-authority) — unlike `/proc/<pid>/sched`'s OQ-4-gated *per-thread*
internals. Reads no per-CPU lock: `idle_ns` is a coherent `__atomic` snapshot of
its sole (per-CPU idle) writer, `capacity` is boot-static.

The **on-CPU thread** per core (§3.4's `/dev/sysstat` "who is on each CPU") is a
v1.x add — it needs a per-CPU current-thread pointer + cross-CPU `Thread*`
validation (a UAF concern the meter deliberately sidesteps).

---

## Walk semantics

Mirrors devproc (`32-devproc.md`):

| `cur_path` | `name` | Result |
|---|---|---|
| any | `".."` | go up (to root) |
| `0` (root) | one of `"procs"`, `"memory"`, `"devices"`, `"kernel-base"`, `"sched"` | leaf qid (QTFILE) |
| anywhere else | any | miss |

Multi-step walk supported via the same `walkqid_alloc / spoor_clone / walk_one` loop pattern. nname=0 returns `Walkqid` with `nqid=0` and a clone of c (the cclone idiom).

---

## Implementation

`kernel/devctl.c` (~280 LOC). Structure:

- **Qid encoding** — `CTL_KIND_*` enum + `CTL_QID_ROOT_PATH = 0`.
- **Tiny formatters** (`fmt_udec`, `fmt_sdec`, `fmt_uhex`, `fmt_str`) — duplicated from devproc.c at v1.0; future shared header (`<thylacine/fmt.h>`) when more callers emerge.
- **Per-leaf format generators** — `format_procs`, `format_memory`, `format_devices`, `format_kernel_base`, `format_sched`. Each writes up to `cap` bytes into the caller's stack buffer; returns total bytes that would be produced for offset-aware reads.
- **Per-leaf table** (`g_ctl_leaves[]`) — name + kind + format function pointer. Walk and read both dispatch through this table.
- **`leaf_for_kind`** — linear lookup by kind enum.
- **`walk_one`** — qid-by-qid step dispatch.
- **Vtable functions** — `attach`, `walk`, `open`, `close`, `read`, `write`, plus the standard stubs.

`format_procs` uses a callback-based formatter (`format_procs_cb`) chained through `proc_for_each` — the iteration must hold `g_proc_table_lock` (devproc's `proc_find_by_pid` shows the precedent). The callback writes one row per Proc; if buffer overflows mid-row, `overflow` flag stops iteration cleanly.

Read buffer cap: 512 bytes (vs 256 for devproc). The `procs` listing scales with live-Proc count; 512 bytes accommodates ~30 procs at typical row width before truncation. v1.0's live-Proc count is ~5 — comfortable headroom. Future scaling: switch to a `kmalloc`-backed buffer when the listing might exceed 4 KiB.

---

## Spec cross-reference

P4-D is impl-only — no new TLA+ module. The lock-discipline cross-references:

- **`g_proc_table_lock`** is held by `proc_for_each`'s walker. `format_procs_cb` runs inside the lock; must NOT re-enter `rfork` / `exits` / `wait_pid` / `proc_find_by_pid`. The callback uses only pure formatters + reads of stable Proc fields.
- **No bestiary lock** at v1.0 — `dev_register` is called only at boot during `dev_init`. Late-registration support (Phase 5+ hot-plug) needs a `bestiary_lock` to serialize `dev_register` against concurrent `dev_lookup_by_dc/name` and devctl's `format_devices` walk.

When `specs/9p_client.tla` lands at Phase 4+, devctl's `/ctl/9p/` subdirectory will expose 9P session info (per ARCH §9.4); the spec's invariants on tag uniqueness + fid lifecycle will be visible via this Dev.

---

## Tests

`kernel/test/test_devctl.c` — 11 tests:

| Test | Covers |
|---|---|
| `devctl.bestiary_smoke` | Registration: dc='C' + name="ctl" + lookup. |
| `devctl.attach_returns_dir` | Root attach: qid.path=0, qid.type=QTDIR. |
| `devctl.walk_to_each_leaf` | Walk to each of `procs`/`memory`/`devices`/`kernel-base`/`sched`; verify QTFILE + path != root. |
| `devctl.walk_unknown_misses` | Walk to "does-not-exist" returns Walkqid with nqid=0. |
| `devctl.read_procs_format` | Output contains "PID" / "STATE" / "ALIVE". |
| `devctl.read_memory_format` | Output contains "total:" / "free:" / "reserved:" / "pages". |
| `devctl.read_devices_format` | Output contains DC+NAME header + every registered Dev's name. |
| `devctl.read_kernel_base_format` | Output contains "kernel_base:" / "kaslr_offset:" / "seed_source:" / "0x". |
| `devctl.read_sched_format` | Output contains "runnable:". |
| `devctl.write_rejected` | Write to ctl returns -1 (admin commands deferred). |
| `devctl.read_dir_returns_neg1` | Root directory read returns -1 (readdir deferred). |

---

## Status

| Component | State |
|---|---|
| `kernel/devctl.c` + devctl Dev (dc='C') | Landed (P4-D) |
| `/ctl/procs` (proc listing via proc_for_each) | Landed (P4-D) |
| `/ctl/memory` (phys allocator stats) | Landed (P4-D) |
| `/ctl/devices` (bestiary listing) | Landed (P4-D) |
| `/ctl/kernel-base` (KASLR diagnostics) | Landed (P4-D) |
| `/ctl/sched` (runnable count + wc stats) | Landed (P4-D) |
| `/ctl/cpu` (per-CPU idle_ns + capacity; prowl-3b) | Landed (prowl-3b) |
| Walk dispatch + offset-aware read | Landed (P4-D) |
| In-kernel tests | 11 covering registration + per-leaf reads + walk misses + write rejection |
| Bestiary count | 7 (devnone + cons + null + zero + random + proc + ctl) |
| Nested directories per ARCH §9.4 (`kernel/`, `sched/`, `irq/`, `mm/`, `security/`, `log/`) | Held to a Phase 4+ chunk that extends qid encoding |
| `/ctl/9p/` (9P session stats) | Held to Phase 5+ alongside 9P client |
| `/ctl/proc-events/exit` (driver supervision subscription) | Held to P4-M (driver supervision) |
| Admin commands via writes | Held to Phase 5+ alongside syscall surface |
| `_Static_assert` on output buffer cap | Not added — tests verify content fits; cap is documented in the file generator. |

---

## Known caveats / footguns

### Writes return -1 at v1.0

The Plan 9 idiom is "write commands to /ctl/<file>" (e.g., `echo scrub > /ctl/mm/stratum`). v1.0 P4-D rejects all writes; admin commands need either (a) the syscall surface to dispatch into the kernel, or (b) the future `/ctl/proc-events/` subscription model. Tests pin -1; replacing it requires explicit code change.

### `/ctl/procs` truncates if Proc count > ~30

512-byte buffer cap. At ~16 bytes per row this seats roughly 30 procs before truncation. Phase 5+ either:
- Bumps the buffer cap.
- Switches to `kmalloc`-backed dynamic sizing.
- Adds pagination via offset.

The contract `read at offset off` already supports pagination — caller can re-read with off += got — but the per-call buffer cap currently bounds total bytes producible. Ratchet up the cap when needed.

### `/ctl/devices` is order-sensitive

Lists devs in registration order. The bestiary registration order is fixed in `kernel/dev.c::dev_init`. Tests check for substrings, not exact lines, so the order doesn't lock the test contract — but operators reading /ctl/devices see a deterministic order.

### `format_procs_cb` runs under `g_proc_table_lock`

`proc_for_each` holds the lock for the entire DFS. Long-running iteration would block all proc-table mutations (rfork, exits, wait_pid) for the duration. v1.0 the proc tree is shallow (kproc + ~5 children at most); cost is ~50µs total. Phase 5+ syscall surface might need a snapshot-based iteration to avoid the lock-held latency spike.

### `kernel/devctl.c` duplicates `fmt_udec` / `fmt_sdec` / `fmt_str` from devproc.c

Two ~30-line copies of the same formatters. Worth factoring to `<thylacine/fmt.h>` when the third caller emerges (likely P4-E ramfs `stat` formatter or P4-G irqfwd diagnostics).

---

## References

- `docs/ARCHITECTURE.md` §9.4 — the canonical /ctl/ layout (this implements the leaf-file subset).
- `docs/ROADMAP.md` §6.1 — Phase 4 deliverables (dev/ctl among them).
- `docs/reference/30-dev-spoor.md` — Dev vtable + Spoor lifecycle (substrate).
- `docs/reference/32-devproc.md` — devproc (prior directory Dev; same walk pattern).
- `arch/arm64/kaslr.h` — `kaslr_kernel_high_base` / `kaslr_get_offset` / `kaslr_get_seed_source` accessors used by `format_kernel_base`.
- `kernel/include/thylacine/proc.h` — `proc_for_each` (added in P4-C; used by `format_procs`).
- `kernel/include/thylacine/sched.h` — `sched_runnable_count` accessor.
- `mm/phys.h` — `phys_total_pages` / `phys_free_pages` / `phys_reserved_pages` accessors.
