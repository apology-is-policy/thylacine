# PROWL-DESIGN.md — the scheduler-aware process monitor + the kernel telemetry it reads

Status: **SIGNED OFF 2026-07-22 — §7 resolved. prowl-1 (substrate) + prowl-2
(tool MVP) + prowl-3 (the scheduler view: 3a counters + 3b `/proc/<pid>/sched` +
`/ctl/cpu` + 3c per-CPU bars + detail pane) LANDED; prowl-5 (the focused audit)
CLOSED CLEAN (0 P0 / 0 P1 / 0 P2 / 4 P3).** Remaining: prowl-4 (the manager:
stop/cont + tree). Building telemetry-first (prowl-1..5, §6).
Landed per the CLAUDE.md design-conversation pattern (research → doc → surface
forks → signoff → bind ARCH → build). The tool is `prowl`; the kernel telemetry
it reads is `/proc/<pid>/{status,sched}` + `/ctl/{procs,cpu}`.

**As-built (prowl-1):** `Thread.run_ns`/`switched_in_at` (the ctx-switch on-CPU
accounting), `Proc.name` (the basename of the resolved binary, stamped at
`exec_setup_from_spoor`; kproc/joey stamp their literals), `proc_cpu_ns` (the
per-Proc sum under `g_proc_table_lock`), `/proc/<pid>/status` at Plan 9 parity
(name/cpu_ns/ppid/principal/gid), and `/ctl/procs` with NAME + CPU_NS columns
(`DEVCTL_READ_BUF` 512 → 2048). READ-ONLY (no scheduling decision reads run_ns);
no new syscall, no new §28 invariant. Validated by `proc.cpu_ns_accounting` +
the SMP gate (default+UBSan × smp4/smp8, 0 corruption). The audit-trigger row is
ARCH §25.4 (Scheduler, prowl-1 addendum); the focused audit is prowl-5. As-built
detail: `docs/reference/15-scheduler.md` (§ on-CPU accounting) +
`32-devproc.md` + `33-devctl.md`.

**As-built (prowl-2):** the native `prowl` Kaua TUI (`usr/prowl/`, like nora) --
polls `/ctl/procs` (the all-pids table + cumulative `cpu_ns`) + `/ctl/sched`
(`cpus: N`), diffs `cpu_ns` across ~1.5 s ticks for %CPU (the htop method, on
`time::Instant` -- the vDSO monotonic clock), and renders an **aggregate** CPU
meter (per-core count-normalized) + a sortable process list (pid/name/%cpu/mem/
threads/state) with cursor-follows-PID selection. As a manager it kills the
selected process via `/proc/<pid>/ctl` (I-26 two-axis gate; NO new authority --
a confined user kills only its own; confirm-gated `k`->`y`). Console discipline
is the nora contract (I-27): owns fd 1 (Terminal) + fd 0 (PollSource), never
touches consctl; ut's raw-mode dance + crash-restore backstop (prowl in ut's
`is_raw_command`). **Pure userspace -- kernel byte-unchanged; no new syscall, no
new §28 invariant.** The **per-CPU** meters + the scheduler-introspection view
are prowl-3 (they need the `/ctl/cpu` leaf, not built yet). Validated by
`tools/interactive/prowl.exp` (the LS-CI E2E: launch + raw-mode dance +
telemetry render + nav/sort + quit + console restore) + boot OK + the intact
suite. As-built detail: `docs/reference/144-prowl.md`.

---

## 1. Mission

Thylacine has rich, real telemetry the operator cannot see. The scheduler tracks
EEVDF virtual deadlines, tickless park/wake counts, work-conservation starvation,
per-CPU idle — but only in **global** aggregate (`/ctl/sched`), and `/proc/<pid>`
reports a process's *state* but **not its CPU time and not even its name**. So
the one question an operator asks first — "what is burning my cores?" — has no
answer short of a bisect.

The motivating case is concrete and recent: the HVF-idle-~256% regression
(`docs/DEBUGGING-PLAYBOOK.md §6.17`) took a multi-hour git-bisect to pin to a
leaked busy-yielding debug fixture. With a per-process CPU view it would have been
a five-second read: "`ambush-child` ×2, ~100% each." Every future floor-chase
under the "go build is the oracle" mandate wants the same visibility.

This is two layers, and they are separable:

1. **The telemetry substrate (kernel):** per-process CPU accounting + names +
   a scheduler-introspection view, exposed through the existing `/proc` and
   `/ctl` synthetic filesystems. *The filesystem is the API* — no new syscall.
2. **The tool (`prowl`):** a native `libthyla-rs` TUI on Kaua (the LS-7 console-TUI
   substrate) that reads that telemetry, renders it, and — as a *manager* — acts
   on it (kill / stop / cont) through the existing `/proc/<pid>/ctl` surface.

The identity fit is exact: filesystem-is-the-OS, the shell is the UI. This is
Plan 9's `/proc` model, brought to parity and then past it.

---

## 2. Prior art (the research the fork needs)

### 2.1 Plan 9 — the heritage

Plan 9 puts the whole process interface in `/proc/<pid>/`: `status`, `ctl`,
`args`, `ns`, `segment`, `wait`, `mem`, `text`, `note`/`notepg`, `kregs`, etc.
`ps(1)` is a thin reader of `/proc/*/status`, whose fixed-width line carries
**name, owner, state, user/sys/real CPU times, memory, base-priority, priority**.
Per-CPU/system-wide counters live in `/dev/sysstat` (context switches, syscalls,
faults, interrupts, load, idle) and `/dev/swap`. There is no `htop` in stock
Plan 9 — but the *substrate* is exactly `/proc/*/status` + `/dev/sysstat`, read by
`rc`/`acid`/`ps`. Control is a write to `/proc/<pid>/ctl` (`kill`, `stop`,
`start`, `hang`, ...).

Thylacine already has the skeleton (`/proc/<pid>/{status,ctl,mem,regs,kstack,...}`
per devproc + the I-39 debug arc; `/ctl/procs` + `/ctl/sched`) — but our
`/proc/<pid>/status` is a strict **subset** of Plan 9's: it lacks CPU times and
the name. The heritage-faithful first move is to bring `status` to Plan 9 parity.

### 2.2 Modern SOTA

- **htop / btop / glances:** poll `/proc/stat` (per-CPU jiffy buckets:
  user/nice/sys/idle/iowait/irq/softirq/steal) + `/proc/<pid>/stat`
  (utime/stime/state/nice/priority/starttime/num_threads) + `/proc/<pid>/cmdline`,
  and **diff the cumulative counters between polls** to derive %CPU. UI: per-core
  meters, a sortable/tree process list, and control (kill/renice). btop adds
  net/disk/gpu graphs; glances adds sensors. All of them read the Linux `/proc`
  text FS — the same shape we already have.
- **Fuchsia / Zircon:** `ps`/`top` call `zx_object_get_info`
  (`ZX_INFO_TASK_RUNTIME`, `..._STATS`) — the *object-info syscall* model, giving
  `cpu_time` per task. Capability-clean but **not** a filesystem interface; it
  does not map onto Thylacine's per-Proc, FS-mediated model (the CLAUDE.md
  research note: Linux/Fuchsia ambient/object answers often don't fit our shape).

### 2.3 The gap they all share — and the Thylacine fusion

Every mainstream monitor shows **%CPU + state** because that is all the OS cleanly
exposes. None is **scheduler-aware**: Linux's CFS/EEVDF virtual-runtime and lag
are not surfaced per-process (only via the obscure, global `sched_debug`), so no
`top` can tell you *why* a process is scheduled the way it is — only that it is.

Thylacine's scheduler is unusually legible from the inside: per-thread EEVDF
`vd_t` (virtual deadline) + lag, the band (INTERACTIVE/NORMAL/IDLE), the tickless
park/wake machinery with **wake-source attribution** (IPI vs one-shot), the
work-conservation **starved-park** accounting (`sched_wc_stats`), per-CPU idle.
Surfacing that **per-process** over the Plan 9 `/proc`+`/ctl` model, in a Kaua
TUI, with control via `/proc/<pid>/ctl`, is a combination that does not exist in
the wild. A monitor that reports "PID 1853 `ambush-child`: 99% CPU, band NORMAL,
99% self-woken (busy-yield storm)" would have *named this session's
bug on sight*. (As-built precision, prowl-5 F1: for that *solo*-yielder-on-an-idle-
system case the lead signal is **%CPU**/`run_ns` — the #33 `sched_yield_hint`
fast-path skips the switch, so a solo yield storm does *not* move `nsched`; the
per-thread `nsched` [dispatch churn] + `parks` counters add the *character* of a
*contended* or thrashing workload, not the solo-idle case. See
`docs/reference/15-scheduler.md`.) That scheduler-introspection angle is the novel contribution
(§8) — the rest is competent execution of a known shape.

---

## 3. The telemetry substrate (kernel)

### 3.1 Per-thread `run_ns` accounting — the load-bearing addition

VERIFIED: nothing in the tree tracks cumulative on-CPU time (grep of
`sched.c`/`thread.h`/`proc.h` for `run_ns`/`cpu_time`/`utime` is empty). This is
the one genuinely new kernel mechanism.

- A `u64 run_ns` accumulator on `struct Thread`, plus a `u64 switched_in_at`.
- At **switch-in** (in `sched()`, around `cpu_switch_context`): `switched_in_at =
  timer_now_ns()`.
- At **switch-out**: `run_ns += timer_now_ns() - switched_in_at`.
- Per-process CPU time = sum of `run_ns` over `p->threads` (computed at read time
  in devproc, under the proc-table lock, atomics on the counters).
- **Cost:** one `CNTVCT` read + one add + a couple of stores on the context-switch
  path — the tickless/timer paths already read `CNTVCT`, so the marginal cost is
  an add and two stores. Negligible, but it *is* the hot path.
- **Read-only:** no scheduling decision reads `run_ns`. EEVDF placement, I-8
  (liveness), I-17 (latency), the `vd_t` math — all untouched. This is telemetry,
  not policy.

The reader derives **%CPU** the htop way: two polls, `Δrun_ns / Δwall` per proc.
No instantaneous-rate state is kept in the kernel; the tool diffs cumulative
counters. (This is why the counter must be *cumulative + monotonic*.)

`OQ-2` — **total `run_ns` vs a user(EL0)/kernel(EL1) split.** A u/s split (Plan 9
+ htop both show it) requires stamping at every EL0↔EL1 boundary (syscall + fault
entry/exit), which is more hot-path work and more state on an already-audit-heavy
surface. Recommendation: **total `run_ns` at v1.0** (the single %CPU number is
what an operator reads 99% of the time); the u/s split is a clean v1.x refinement
that adds fields without changing the model.

### 3.2 Process names

VERIFIED: `struct Proc` has no name field (only `proc_fault_terminate(const char
*name, ...)`, a parameter). `exec_setup*` already receives the program name /
`argv[0]`. Store a bounded `char name[PROC_NAME_MAX]` (the basename of the execed
path) on `struct Proc` at exec, expose it in `/proc/<pid>/status` and add it as a
column to `/ctl/procs`. Cheap, and it removes the "which pid is what" guesswork
that made this session's bisect necessary.

### 3.3 The `/proc/<pid>` layout — Plan 9 parity, then the scheduler view

- **`/proc/<pid>/status`** (bring to Plan 9 parity): `name`, `state`, `threads`,
  **`cpu_ns`** (Σ run_ns), `mem` (pages — already in `/ctl/procs`), `ppid`,
  `principal`/`gid`. A stable, greppable, fixed-shape text block (the existing
  `fmt_*` style).
- **`/proc/<pid>/sched`** (NEW — the differentiator): the scheduler-introspection
  block — `band` (INTERACTIVE/NORMAL/IDLE), `vd_t`/`lag` (EEVDF), `parks`,
  `starved_ns`, `wake_ipi`/`wake_oneshot` (source split), `on_cpu` (CPU index or
  -1), `migrations`, `preempt_count`. Most of this is per-thread state that
  already exists in the scheduler; the new work is per-thread counters
  (`parks`/`starved_ns`/`wake_*`/`migrations`) that today are only global in
  `sched_wc_stats`, promoted to per-thread accumulators (same cheap-add pattern
  as `run_ns`).

### 3.4 Per-CPU — the `/dev/sysstat` analog

For the per-core meters the tool needs per-CPU utilization + who is on each CPU:
per-CPU `idle_ns` (today `sched_wc_stats.idle_ns` is global — the idle park
already runs per-CPU, so stamping it per-CPU is a small extension), the on-CPU
thread (pid+name), per-CPU runnable depth, per-CPU park/wake counts.

`OQ-3` — **surface it as a new `/ctl/cpu` leaf, or a Plan-9-faithful
`/dev/sysstat`?** Recommendation: a new **`/ctl/cpu`** leaf (keeps `/ctl/sched`
as the *global summary* it is today; per-CPU is a distinct concern; stays inside
the `/ctl` admin tree). `/dev/sysstat` is the heritage-faithful alternative and
is a legitimate call — surfaced as the fork.

### 3.5 The visibility gate

`OQ-4` — **who may read what.** Today `/ctl/procs` lists *every* pid's state
(Plan 9 all-pids-visible; visibility-not-authority; #57a). Extending it with
name + %CPU keeps that posture — coarse metadata stays all-visible, which is what
a whole-system monitor needs. The **deep** per-process view (`/proc/<pid>/sched`,
per-thread internals, another proc's `cpu_ns` detail) is the question: Plan 9
gates `/proc` detail by *ownership*; Thylacine's I-39 gates cross-proc
*inspection*. Recommendation: **the summary (name/%CPU/state/mem) stays
all-visible** (the `prowl` overview, exactly `/ctl/procs`'s posture); **the deep
internals follow an owner-or-`CAP_HOSTOWNER` gate** (a full-system monitor is the
operator's tool; a confined user still sees the overview + full detail on its own
processes). This composes I-1 (isolation) + the existing `/ctl` posture; it adds
no new §28 invariant.

---

## 4. The tool — `prowl`

### 4.1 A Kaua TUI (native libthyla-rs)

Built on Kaua (LS-7; `usr/lib/kaua`), the same substrate as `nora`. Polls `/proc`
+ `/ctl` on a cadence (default ~1–2 s, configurable), diffs `cpu_ns` for %CPU. A
buggy `prowl` corrupts only its own screen — it reads the FS and validates
nothing on the kernel's behalf (the kernel gates the reads).

### 4.2 The view

- **Header:** per-CPU meters (utilization from per-CPU `idle_ns`) + a global line
  making the tickless/work-conservation health visible — parks/s, starved-%,
  wake-source split (the `#299`/`#363` idle-cost axis, finally on screen).
- **Process list:** sortable columns — `pid`, `name`, `%cpu`, `mem`, `threads`,
  `band`, `parks/s`, `state`. The band/parks columns are the differentiator.
- **Tree view:** parent→child (needs `ppid` exposed, §3.3).
- **Detail pane:** a selected process's `/proc/<pid>/sched` internals
  (`vd_t`/lag, wake-source, starved, per-thread `run_ns` + `on_cpu`).

### 4.3 The manager (control)

- **kill / killgrp** via `/proc/<pid>/ctl` — already implemented, I-26 two-axis
  gated (owner OR `CAP_HOSTOWNER`/`CAP_KILL`). prowl adds **no new authority**; a
  user kills only what it is already authorized to.
- **stop / cont** via the PTY job-control ctl surface (the SIGTSTP/SIGCONT path).
- **reband / priority** — a scheduler-control `/proc/<pid>/ctl` verb is a v1.x
  seam (a real privilege surface; deferred, called out here so it isn't smuggled
  in).

---

## 5. Invariants + audit posture

- **`run_ns`/counters are read-only telemetry.** No scheduling decision reads
  them; EEVDF/I-8/I-17 correctness is untouched. The obligation is *cheapness on
  the ctx-switch hot path* (a `CNTVCT` read already present + an add + two stores)
  and *SMP-correctness of the counters* (per-thread, atomic; a cross-proc reader
  holds the proc-table lock and takes atomic snapshots — the `/ctl/procs` #57a
  pattern). `sched.c` is an audit-trigger surface, so the addition gets the
  scheduler treatment (the focused audit + the SMP gate), even though it changes
  no decision.
- **The visibility gate (OQ-4) is the privilege obligation:** the deep-internals
  gate must not leak another process's state to an unauthorized reader (composes
  I-1 + I-39 + the `/ctl` visibility-not-authority posture).
- **No new syscall** (the FS is the API — Plan 9). **No new §28 invariant**
  (composes I-1 / I-26 / I-39 / the `/ctl` posture). The ARCH §25.4 audit-trigger
  table gains a row for the ctx-switch `run_ns` accounting **at impl time, after
  signoff** — not pre-bound here.

---

## 6. Phasing

- **prowl-1 (substrate MVP): LANDED.** `run_ns` accounting + proc names +
  `/proc/<pid>/status` Plan 9 parity + `/ctl/procs` name/CPU_NS columns.
  Kernel-only; the data a basic top needs. `proc.cpu_ns_accounting` kernel test +
  the SMP gate (ctx-switch path, 0 corruption). The kernel exposes cumulative
  `cpu_ns`; the tool derives %CPU by diffing across polls.
- **prowl-2 (tool MVP): LANDED.** the Kaua `prowl` TUI — an **aggregate** CPU
  meter (per-CPU bars land with `/ctl/cpu` in prowl-3) + the process list + %CPU
  (diffing `cpu_ns` across ticks) + confirm-gated kill via `/proc/<pid>/ctl`. An
  htop-equivalent, on-device; pure userspace. `usr/prowl/` +
  `tools/interactive/prowl.exp`; as-built `docs/reference/144-prowl.md`.
- **prowl-3 (the scheduler view)** — the "better than htop" differentiator, split
  into three sub-chunks:
  - **prowl-3a (the counter accounting): LANDED.** Four per-thread scheduler
    counters (`Thread.nsched`/`nsleeps`/`nmigrations`/`last_cpu`) stamped at the
    same single switch chokepoint as `run_ns`, plus per-CPU `CpuSched.idle_ns`
    (the `/ctl/cpu` meter denominator, read via `sched_cpu_idle_ns`). READ-ONLY
    telemetry (no decision reads them); `scheduler.prowl_counters` + the SMP gate.
    As-built: `docs/reference/15-scheduler.md` (§ per-thread scheduler counters).
  - **prowl-3b (the FS surfaces): LANDED.** `/proc/<pid>/sched` (`devproc.c`
    `format_sched` — the per-thread block, gated **owner-or-`CAP_HOSTOWNER`** at
    the read site per OQ-4, `devproc_sched_authorized`) + the `/ctl/cpu` per-CPU
    leaf (`devctl.c` `format_cpu` — idle_ns + capacity, all-visible). Tests
    `devproc.{sched_gate_predicate,read_sched_format}` + `devctl.read_cpu_format`.
    As-built: `docs/reference/32-devproc.md` + `33-devctl.md`.
  - **prowl-3c (the tool): LANDED.** Per-CPU meter bars (from `/ctl/cpu`, one
    mini-bar per core, replacing the prowl-2 aggregate meter) + the `d`-toggled
    per-thread detail pane (the selected process's `/proc/<pid>/sched`, gate-aware
    -- shows "unavailable" when OQ-4 denies). The design's band/parks *list
    columns* moved into the **detail pane** instead: they are per-thread (a proc
    has N bands) and OQ-4-gated (mostly-blank as list columns for a normal user),
    so the detail pane is their correct home; the list stays the all-visible
    `/ctl/procs` summary. `usr/prowl/{sample,main,ui}.rs` +
    `tools/interactive/prowl.exp` (the `d` detail leg). As-built:
    `docs/reference/144-prowl.md`.
- **prowl-4 (the manager):** stop/cont control + the tree view (+ the reband seam
  if voted in).
- **prowl-5 (audit): CLOSED.** The focused adversarial audit of the prowl-3 arc
  (the visibility gate + the hot-path cost + the counter SMP-safety) — a dedicated
  Fable-5-max prosecutor + a concurrent self-audit, both converging on **sound**:
  **0 P0 / 0 P1 / 0 P2 / 4 P3, NOT dirty.** The four P3s (all fixed): F1 the
  `nsched` "names a busy-yield storm" doc claim (defeated by the #33 yield
  fast-path — a *solo* yielder skips the switch, so `%CPU`/`run_ns` names the
  HVF-idle case and `nsched` is the churn signal); F2 `/ctl/cpu` rendered a
  never-online CPU as 100% busy (gate on `g_cpu_online`); F3 `format_sched`'s
  plain `state`/`band` reads → `__atomic` (the comment's own discipline); F4 the
  OQ-4 *deny* wiring had no failing test → factored `devproc_sched_read_gated` +
  the `devproc.sched_read_gated` revert-probe (proven non-vacuous). The
  READ-ONLY-telemetry claim, the stamp single-writer discipline, the OQ-4 gate,
  walk-safety, and the buffer discipline all held. Closed list:
  `memory/audit_prowl5_closed_list.md`.

---

## 7. Resolved decisions (signed off 2026-07-22)

- **OQ-1 — the name → `prowl`.** A predator surveying its territory; reads as a
  verb (`prowl`, `prowl -s cpu`). (The kernel telemetry surfaces are named for
  what they are — `/proc/<pid>/status`, `/proc/<pid>/sched`, `/ctl/cpu`; `prowl`
  is the tool.)
- **OQ-2 — `run_ns` → total at v1.0.** The single %CPU number is what an operator
  reads; the user/kernel (EL0/EL1) split is a clean v1.x field addition (no model
  change).
- **OQ-3 — per-CPU surface → a new `/ctl/cpu` leaf.** Keeps `/ctl/sched` as the
  global summary and `/ctl` cohesive. (`/dev/sysstat` was the heritage-purist
  alternative; not chosen.)
- **OQ-4 — visibility → summary all-visible, deep internals gated.** The summary
  (name / %cpu / state / mem) stays all-visible like `/ctl/procs` today; the deep
  per-process view (`/proc/<pid>/sched`, per-thread internals, another proc's
  detail) follows an **owner-or-`CAP_HOSTOWNER`** gate (composes I-1 + the `/ctl`
  visibility-not-authority posture; no new §28 invariant).
- **OQ-5 — v1 scheduler-view scope → the actionable set.** `band` /
  `parks-per-s` / `wake-source` / `starved` ship at v1 (the set that named this
  session's bug); the raw EEVDF `vd_t`/lag is expert-only → v1.x.

---

## 8. Novel contribution

The scheduler-introspection angle — surfacing per-process EEVDF/tickless/
work-conservation internals (band, `vd_t`/lag, parks, starved-time, wake-source)
that no mainstream monitor exposes, over the Plan 9 `/proc`+`/ctl` filesystem
model, with a Kaua TUI and control via `/proc/<pid>/ctl` — is genuinely
unoccupied. htop/btop are %CPU monitors; Fuchsia's is an object-info syscall
monitor; none is *scheduler-aware over a filesystem telemetry model*. Whether
this rises to a `NOVEL.md` lead position (vs. a strong observability tool) is
**the user's call** — recorded here as a candidate per the research discipline
(record the synthesis even when v1.0 defers it), not unilaterally added to the
nine leads.

---

## 9. Cross-references

- `docs/DEBUGGING-PLAYBOOK.md §6.17` — the HVF-idle hunt that motivated this (the
  visibility gap made concrete).
- `docs/KAUA.md` — the console-TUI substrate the tool builds on.
- `docs/TICKLESS-IDLE.md` + ARCH §8.6 — the `sched_wc_stats` / wake-source
  machinery this surfaces per-process.
- ARCH §9.4 (`/proc`, `/ctl`) + `docs/reference/32-devproc.md` /
  `33-devctl.md` — the synthetic-FS surfaces extended here.
- `tools/ci-idle-gate.sh` — the idle gate that, with `run_ns`, becomes
  self-diagnosing (names the spinner instead of only flagging one).
