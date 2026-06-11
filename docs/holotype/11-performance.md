# HOLOTYPE RW-11 — Cross-cut: Performance (inventory + comparison)

**Status**: CLOSED (see §7 for the verification posture)
**Tier**: STANDARD. **No perf rework landed in-arc** — every finding names
(mechanism, measured-or-reasoned cost, SOTA alternative, scripture fit,
suggested tier) and is REGISTERED, not fixed; the only in-arc edits are
trivially-mechanical scripture/comment honesty fixes (§6.1).
**Tip at review start**: `85f1868`.
**Reviewers**: 4 × Fable `holotype-reviewer` (R1 memory hot paths / R2
sched-locks-tick / R3 IPC-9P-FS / R4 boot-entry-uaccess; all
`claude-fable-5` MODEL start==end, no fallback) + the main-loop empirical
leg (3 timestamped HVF boots + 1 TCG boot) + self-audit.
**Headline**: **0 soundness findings across ~31 perf findings.** Four of the
seven measurable budget rows are MET on the budget's own claimed substrate
(HVF) — syscall floor 83 ns measured, 9P loopback ~24 µs floor, first
banner 89 ms, IRQ-path p99 reasoned-fits — and the audited soundness
mechanisms (elected reader, #844 snapshot, SQPOLL park, ASID fast path,
die-check) are cheap or SOTA-matching. The costs concentrate in (a) the
**no-wake-preemption slice cliff** (SA-1: up to 6 ms wake-to-run under
load — empirically pinned at exactly the 6-tick slice; V1.0-RISK for the
p99/p99.9 budget cells), (b) the **production-boot shape** (the 825-test
suite + joey's argon2-bearing probe ladder are unconditional in the only
build that exists — the kernel's own `boot-time:` line self-reports
1249–1265 ms vs the 500 ms target; V1.0-RISK as a build-shape decision),
and (c) a ranked V1.X backlog headed by the **per-byte uaccess** (a
function call per byte on every IO copy), msize=4096, and the
coarse-global-lock set (torpor / proc-table / zone / SLUB).
**Measuring stick**: VISION §4.5 (the committed v1.0 tail-latency budget);
VISION §5 ranks Performance 6th — enforced by budget, never at the expense
of correctness/security/auditability.

---

## 1. The empirical baseline (budget × measured)

Method: 3 timestamped HVF boots + 1 TCG boot of the dev kernel (tip
`85f1868`, tests compiled in), Apple Silicon host, QEMU `-smp 4`, the
repo-default launchers. HVF is the budget's own claimed substrate
(VISION:168 "achievable on QEMU + Hypervisor.framework on Apple Silicon");
absolute bare-metal numbers remain future work. Logs:
`/tmp/rw11-boot-{hvf-1,hvf-2,hvf-3,tcg-1}.log` (session-local).

| VISION §4.5 row | Budget (p50 / p99 / p99.9) | Measured (HVF unless noted) | Verdict |
|---|---|---|---|
| Boot → UART banner | — / — / 500 ms | first banner line at **0.089 s**; the kernel's own `boot-time:` self-measure (which runs THROUGH the in-line 825-test suite) = **1249–1265 ms** (TCG 2691 ms) — the non-test fraction is ~90–100 ms | **non-test fraction MET (5×)**; the row is **unmeetable in the only build that exists** (R4-F1, V1.0-RISK build-shape) |
| Boot → login prompt | — / — / 3 s | boot-OK **3.05–3.07 s** provisioned-pool, 4.62 s first-boot enrollment — including the test suite (~1.24 s, irq-bench alone 0.77 s), loom benches (~0.3 s), and joey's ≥7 argon2 derivations in the probe ladder (R4-F2); production-shape ≈ 1–1.5 s | **MET in shape**; not measurable as committed — no production boot config exists (R4-F1/F2; G1) |
| Syscall, no contention | 200 ns / 500 ns / 1 µs | `SYS_LOOM_ENTER` NOP full round trip = **83 ns/op**, identical across 3 boots (TCG 3125 ns) | **MET** on HVF (the NOP enter does strictly more work than getpid) |
| Syscall, kernel-handled (pipe) | 1 µs / 5 µs / 20 µs | no pipe bench exists (G2) | **UNMEASURED**; p99/p99.9 cells at risk via SA-1(b) |
| Process creation | 200 µs / 1 ms / 5 ms | ramfs spawn→EL0→exit→reap ≈ 1–3 ms wall per probe (envelope incl. child runtime); no dedicated bench (G3) | **UNMEASURED** precisely; post-pivot spawns additionally pay ~55 9P RPCs (F2) |
| 9P round-trip (loopback Stratum) | 50 µs / 500 µs / 2 ms | present-proxy `LOOM_OP_WRITE` full path (loom → dev9p client → srvconn rings → stratumd → Stratum FS → back) = **24.4 / 26.9 / 60.6 µs** across boots (TCG 209 µs); FSYNC 0.79–0.98 ms (commit-dominated; no budget row) | **floor MET** (~24 µs), load-sensitive — p50 plausibly met, p99 unmeasured |
| IRQ → userspace driver | 1 µs / 5 µs / 20 µs | irq-bench p50 = **6.0224 / 6.0235 / 6.0236 ms**, σ≈10 µs (TCG 9.10 ms plateau) — the bench saturates at the 6-tick slice quantum; see SA-1 | **NOT measurable as built** — the bench has only ever measured the test-mode slice-wait path |
| Halcyon frame time | 10 / 16 / 33 ms | N/A (Phase 8) | — |

Reproducibility notes: loom NOP 83 ns and irq-bench ~6.023 ms are
boot-invariant to within counter granularity; present-proxy varies 24–61 µs
with host load; first-boot USER_CREATE enrollment costs ~2.08 s/user
(argon2id ×2 wraps + keypair mint — deliberate, C-16 class, not on the
production boot path); login AUTH unwrap ≈ 100–400 ms/login (deliberate).
The pre-HVF TCG bimodality (19–37 s boots, DEBUGGING-PLAYBOOK §6.12) is
extinct under HVF-default: total dev-boot wall-clock 3.05–4.62 s.

## 2. The headline: SA-1 — the slice-quantum wake cliff (and the bench that measures itself)

`THREAD_DEFAULT_SLICE_TICKS = 6` (sched.h:48) × the 1000 Hz tick = 6 ms.
The irq-bench plateau IS this constant. Two distinct findings:

**SA-1a (bench artifact, G4)**: the in-kernel suite runs BEFORE
`sched_set_notify_enabled(true)` (main.c:654 vs :734), so during irq-bench
the same-CPU wake path's idle-peer notify (`ready_on` → `sched_notify_idle_peer`,
sched.c:667-668) early-returns on the disabled gate (sched.c:556). The SPI
targets CPU0 = the busy-yield bench driver; the woken child lands on CPU0's
rq; idle peers sit in WFI with no per-CPU timer (sched.c:544) and no IPI →
the only release is CPU0's slice expiry; the handshake phase-locks to the
slice boundary → delta ≈ exactly one slice (σ≈10 µs). Every irq-bench
number ever logged (the 41-irq-bench.md TCG numbers included) is a
slice-quantum reading under test-mode, not an IRQ-path reading. The
production wake path (notify IPI → WFI exit → `try_steal`) has never been
measured by anything.

**SA-1b (structural, production-real)**: at v1.0 `select_target_cpu`
returns the waker's CPU (sched.c:648/691) and the same-CPU `ready_on`
branch sets no `need_resched` and sends no self-IPI (`need_resched_set` is
the cross-CPU branch only, sched.c:678 — the #866 F1 fix never covered the
same-CPU case); preemption is slice-expiry-only (sched.c:1655). With an
idle peer, `sched_notify_idle_peer` is the escape hatch (µs-scale steal).
With NO idle peer — i.e. under coexisting CPU-bound load, exactly the
regime p99/p99.9 budgets describe — **every rendez wake (pipe reader, 9P
reply, poll, IRQ-to-driver) waits up to a full 6 ms slice** behind the
current thread. That exceeds the kernel-handled p99.9 (20 µs) by ~300× and
the IRQ-to-driver p99.9 likewise. SOTA: Linux wakeup-preemption
(check_preempt at every wake — EEVDF/CFS compare the woken entity against
current) + idle-CPU search in select_task_rq; seL4 priority-compares at
wake. This is the latency face of HT02.2A-F6 (yield-counter "EEVDF"), now
with an empirical magnitude pin. **Tier: V1.0-RISK** for the p99/p99.9
cells of the kernel-handled-syscall + IRQ-to-driver rows. Fix =
wake-preemption + same-CPU need_resched (sched-core rework) — sequenced at
RW-13 with the EEVDF reconcile, NOT landed in-arc.

## 3. The lock-granularity + hot-path inventory (the charter table)

Every charter-named mechanism, as-built, with its SOTA delta and tier.
"Tier" vocabulary: BUDGET-OK (fine; recorded) / WATCH (fine at v1.0 scale;
the breaking scale is named) / V1.X-REWORK (real cost; SOTA named;
post-v1.0) / V1.0-RISK (plausibly blocks a committed budget gate at the
Phase-7 exit — argued, sequenced at RW-13).

| Mechanism | As-built | Cost at scale | SOTA | Tier |
|---|---|---|---|---|
| **torpor_lock** | ONE global spinlock for ALL futex wait+wake, all Procs (torpor.c:70); held across `wakeup()`'s on_cpu spin (documented in-tree, torpor.c:330) | every contended pthread op system-wide serializes; one mid-switch wake stalls all futex traffic | Linux per-bucket `futex_hash_bucket.lock`; per-bucket split does NOT collide with I-9 (register-then-observe lives under per-Thread `wait_lock`) | WATCH → V1.X (R2-F2) |
| **g_proc_table_lock** | one global lock; held across the ENTIRE group-terminate wake-storm + 64-bucket torpor sweep + broadcast IPI (proc.c:1871-1926); every rfork/exit/wait/thread-create/free funnels through it | a kill spike stalls every proc-lifecycle op system-wide; rfork tail-latency queues behind it | Linux `tasklist_lock` rwlock + per-task siglock + RCU iteration + targeted `kick_process` | WATCH → V1.X (R2-F3, R2-F8); narrowing is audit-bearing (I-24, RW-2 F75) |
| **Per-9P-session serialization** | `c->lock` per session + ONE elected reader demuxes all replies (#841) | demux+copy+wake serialized through one reader at high fan-out; ring depth is the real ceiling | MATCHES Plan 9 `mountio` + Linux v9fs single-demuxer — correct design, not a deficiency | BUDGET-OK (R3-F8) |
| **Per-Proc handle-table lock** | every fd syscall: table spinlock + 2 atomics + 32 B snapshot copy (#844 discipline, handle.c:453-485) | N threads of one Proc convoy on ONE lock per I/O op (stratumd-class) | Linux RCU fdtable (lockless `fdget`); preserving #844's TOCTOU close lock-free needs a re-audit | WATCH → V1.X (R3-F7); rework re-audits #844 |
| **Buddy zone lock** | ONE global `g_zone0.lock`; magazines front ONLY orders 0+9 (per-CPU, Bonwick); KP_ZERO correctly OUTSIDE the lock; magazine refill takes the lock PER PAGE (8×/refill, contradicting its header — fixed) | 8-CPU non-{0,9}-order allocation serializes; refill storms 8× intended lock traffic | Linux per-CPU PCP lists front orders 0-3 + bulk refill under ONE hold (`rmqueue_bulk`) | WATCH (R1-F5, R1-F6) |
| **SLUB c->lock** | NO per-CPU fast path — every kmalloc/kfree ≤2048 B takes the per-cache global lock (slub.c:232); ARCH §6.4's "per-CPU active slab... without lock" is design-time fiction (fixed: marked deferred) | 8 CPUs allocating same-cache objects (Vma/Thread/9P bufs) serialize | Linux SLUB `kmem_cache_cpu` + `this_cpu_cmpxchg_double` lockless fast path — the *defining* SLUB mechanism | WATCH → V1.X (R1-F7) |
| **Per-page TLBI on unmap** | `mmu_uninstall_user_range`: per-page `tlbi vaae1is` + `dsb ish` + `isb` under `vma_lock` (mmu.c:1711-1740) | 256 MiB detach ≈ 13-65 ms under the lock (65536 broadcast+wait pairs); 128 KiB munmap ≈ 6 µs (fits) | Linux `mmu_gather`: tlbi-only loop + ONE trailing `dsb ish`; FEAT_TLBIRANGE `tlbi rvae1is` = 1 instr for the range | V1.X (R1-F1); the trailing-dsb half is cheap + invariant-neutral |
| **Per-page TLBI on demand-page install** | broadcast `tlbi` on EVERY invalid→valid first-touch fault (mmu.c:1626; the R7 F129 conservative choice) | W-page warm-up pays W broadcasts (~2-10 ms per 10 k pages); per-fault ~0.2-1 µs (fits alone) | Linux issues NO TLBI on not-present→present (architecture contract) | V1.X (R1-F4) — **audit-bearing; do NOT optimize casually** |
| **The 1000 Hz tick** | every CPU incl. idle ticks at 1 kHz once production-armed; each tick takes the GLOBAL `g_timerwait.lock` (usually-empty scan) = 8 k acquisitions/s at 8 CPUs | idle power/host burn; a global-lock floor under every latency budget | Linux NO_HZ_IDLE (one-shot re-arm to next deadline) + per-CPU timer bases | WATCH → V1.X (R2-F4) |
| **KP_ZERO-everywhere** | zero-on-alloc (phys.c:295, OUTSIDE the buddy lock, `stp xzr` widened — good); BUT eager-commit+zero of the FULL power-of-2-rounded BURROW at attach (129 MiB→256 MiB), under `vma_lock`; SLUB's KP_ZERO is a per-BYTE loop with a per-iteration bound reload (codegen-confirmed); exec double-writes [0,filesz) | 256 MiB attach ≈ 16-43 ms under `vma_lock`; ~2× RAM waste worst-case; security property (no stale bytes to EL0) preserved by ALL the SOTA alternatives | lazy/demand commit (Linux/Fuchsia/seL4 all decouple reserve from commit) + `DC ZVA` + zero-only-the-bss-tail | V1.X (R1-F2, R1-F3, R1-F8, R1-F10); lock-narrowing is the named pull-forward |
| **VMA structure** | single per-Proc `vma_lock` (the #713 invariant) + O(N) sorted list; N≈5-6 native, ~50-100 for a multi-thread musl daemon | every fault serializes per-Proc + O(N) walk under the lock; compounds with the F1/F2 multi-ms holds | Linux maple tree + per-VMA locks (the per-VMA half is audit-bearing vs #713); 26-vma.md already anticipates the O(log N) tree | WATCH → V1.X (R1-F9) |
| **uaccess granularity** | `uaccess_load_u8`/`store_u8` are the ONLY primitives — a FUNCTION CALL PER BYTE (fixup table keyed per-instruction); every read/write/open/path copy loops it | ~20-40 µs per 4 KiB transfer — the single largest removable cost on every IO path; full-4KiB pipe ops blow the <1 µs p50 on this alone | Linux block `copy_*_user` with ONE exception-table entry spanning the loop | **V1.X-REWORK, top priority** (R3-F5; deepens HT03.R1-H4) |
| **msize / ring sizing** | client proposes 4096 on BOTH attach paths → negotiated 4096 (Stratum offers 8 MiB); iounit 4085; `SRVCONN_RING_CAP` 8192 ≈ 2 frames | 1 MiB read = 257 RTTs; per-session bulk ceiling ≈ 19-82 MB/s; a 221 KiB spawn binary = ~55 RPCs | Linux v9fs msize 128 KiB-512 KiB default | V1.X (R3-F2+F4, SA); coordinated lift (ring + scratch + reply pool), audit-bearing on the #841/#845 back-pressure model |
| **stalk on dev9p** | one Twalk + one Tgetattr X-search PER COMPONENT + final getattr + Tlopen = ~2N+2 RPCs per N-component open, EVERY time (no walk batching — wire supports 16 names; no dcache) | `/a/b/c/d` open ≈ 10 RPCs ≈ 0.5-2 ms; every deep `cat`/`cd`/spawn pays it | Plan 9 batches up to 16 walk names; Linux v9fs caches dentries (repeat opens ≈ 0 RPCs); the X-search-per-hop is I-28 — the CACHE is the invariant-compatible answer | V1.X (R3-F9) |
| **Context switch** | full GPR + EAGER FP/SIMD save+restore (1 KiB traffic) + unconditional FPCR `isb` on every switch, FP-less kthreads included; ASID fast path = 1 lock-free CAS; NO per-switch TLBI | per-switch fixed overhead dwarfs the GPR save; switch-heavy IPC pays it twice per round trip | Linux lazy FP (`TIF_FOREIGN_FPSTATE`); seL4 lazy FPU trap | WATCH → V1.X (R2-F5); conditional-FPCR-ISB is the trivial half |
| **Spinlock algorithm** | raw TTAS xchg (LSE-patched), relaxed-load inner spin; NO fairness | unfair under 8-CPU contention on the three global locks; O(N) cacheline bounce per handoff | Linux qspinlock (MCS: FIFO + per-CPU spin cacheline) | WATCH → V1.X (R2-F6); shrink the contended regions first |
| **Run queue** | "run_tree" is a sorted doubly-linked LIST; `insert_sorted` O(N) — the yield path always walks to the tail; `pick_next` O(1) | at N runnable per CPU, every switch does an O(N) walk under `cs->lock` | Linux EEVDF augmented rbtree, O(log N) + cached leftmost | WATCH → V1.X (R2-F7); folds into the EEVDF lift |
| **Group-terminate IPI** | `smp_resched_others` broadcasts to ALL CPUs (smp.c:175), under `g_proc_table_lock` | ncpus-1 full IRQ entries; bystander CPUs discover NULL `group_exit_msg` | Linux `kick_process` targets `task_cpu(p)` (needs a per-Thread cpu stamp) | WATCH → V1.X (R2-F8); composes with I-18 |

## 4. Findings (merged: 4 reviewer bands + self-audit; full prosecution
chains in the session reports `/tmp/holotype-rw11-R{1..4}.md`)

Convergence map: SA-1 ≡ R2-F1 (independent, same chain + magnitude);
SA-msize ≡ R3-F2+F4 ≡ R4-F11; R3-F5 ≡ R4-F3 (uaccess); R2-F5 ≡ R4-F7
(eager FP); R2-F4 ≡ R4-F12a (tick); R1-F10 ⊂ R4-F4 (exec); G1 ≡ R4-F1/F2.
Five prosecutors, four independent multi-way convergences.

### V1.0-RISK (sequenced at RW-13; none landed in-arc)

- **HT11.SA-1 / R2-F1 — the no-wake-preemption slice cliff** (§2). Same-CPU
  wakes never preempt; under saturation every rendez wake waits up to the
  full 6 ms slice (empirically pinned: irq-bench = 6.0224–6.0236 ms across
  3 boots, σ≈10 µs = exactly `THREAD_DEFAULT_SLICE_TICKS`×tick). Blows the
  kernel-handled + IRQ-to-driver p99/p99.9 cells ~300× in the regime those
  cells describe. R2 rates the mechanism WATCH (idle-system p50 fine via
  the notify/steal path; I-17 already honest); adjudicated **V1.0-RISK for
  the Phase-7 budget gate** (the 8-CPU 72 h stress saturates CPUs by
  design). The fix is additive (a check-preempt-on-wake + self
  need_resched; weakens no invariant, composes with on_cpu/I-8/I-21) and
  folds naturally into the RW-13 EEVDF reconcile (HT02.2A-F6).
  **LANDED `@fb5e776`** (#60, the D4 pre-rc emerge): user-voted option C =
  realize the INTERACTIVE band (ARCH 8.3). Same-CPU wake-preempt + the
  syscall-return preempt point + INTERACTIVE for CAP-gated IRQ-waiters + the
  trusted console session (owner/attached); the 2A-F6 reconcile folded in.
  Audit 0 P0/0 P1/1 P2 (F1 over-broad console gate, narrowed)/1 P3 (→#61);
  SMP gate 0-corruption. NOTE the irq-bench number stays slice-quantum until
  the SA-1a bench-mode artifact is addressed (#62) — #60 fixes the production
  wake path, which the bench never measured.
- **HT11.R4-F1 — no production boot configuration**. Every `test/test_*.c`
  is in the unconditional kernel build; `test_run_all()` runs on every
  boot (main.c:654, extinct-on-fail); the kernel's own `boot-time:` line
  self-reports 1249–1265 ms (HVF) vs the 500 ms target. The boot-budget
  rows are structurally unmeetable until a production build shape exists
  (a `THYLACINE_KERNEL_TESTS=OFF` config collides with nothing in §28).
  A build-shape decision, not an optimization.
- **HT11.R4-F2 — joey's probe ladder is production-init**. The serialized
  21-stage ladder (incl. burrow/mallocng torture, loom stress/bench,
  USER_CREATE + RECOVER-E2E + login-E2E = ≥7 deliberate argon2id
  derivations ≈ 0.5–2 s) runs ungated before `t_boot_complete()`.
  Production-gating it is the same decision as R4-F1. NO scripture
  carve-out is needed for the KDF: per-login argon2 runs after credential
  entry — never in the boot-to-prompt window; only the TEST probes put it
  on the boot path. (Corrects the session's earlier carve-out framing;
  corvus's own boot self-test is deliberately argon2-free.)

### V1.X-REWORK (real cost; SOTA named; the ranked backlog)

Priority order (R3's leverage ranking, extended cross-band):

1. **R3-F5 / R4-F3 — per-byte uaccess** (§3 row): a function call per byte
   on EVERY user↔kernel copy (read/write/open-path/spawn-name/cwd);
   ~10–40 µs per 4 KiB. A block `copy_*_user` with one fixup entry
   collides with nothing. Deepens HT03.R1-H4 (and retires its bsearch
   half: the fixup table has 4 entries — moot).
2. **R3-F9 — stalk pays ~2N+2 RPCs per N-component dev9p open**, every
   time (per-hop Tgetattr X-search + one-name Twalk; wire supports 16
   names; no dcache). ~0.5–2 ms per deep open. The invariant-compatible
   answer is a qid/perm cache (I-28 keeps the per-component CHECK; the
   cache feeds it).
3. **R3-F2+F4 / SA / R4-F11 — msize 4096 + 2-frame ring** (§3 row): 257
   RTTs/MiB; ~19–82 MB/s per-session ceiling; ~55 RPCs per 221 KiB spawn
   binary once spawn rides the namespace (#58). Coordinated lift (msize +
   `SRVCONN_RING_CAP` + scratch + reply-pool), audit-bearing on the
   #841/#845 back-pressure model.
4. **R3-F3 — per-RPC `kmalloc(4096, KP_ZERO)` reply buffer** through the
   buddy on every sync 9P op; poolable against the 64-slot tag table with
   zero protocol change.
5. **R1-F1/F2/F3 — the eager-memory cluster** (§3 rows): per-page
   TLBI+DSB unmap loop under `vma_lock` (256 MiB ≈ 13–65 ms); attach
   zeroing under the lock (≈16–43 ms at max); eager-commit + power-of-two
   rounding (129 MiB→256 MiB commit). The batched-`dsb` half of F1 and
   the F2 lock-narrowing are small + invariant-neutral (named
   pull-forward candidates); lazy commit is the real v1.x item.
6. **R3-F7 — handle-table toll**: per-fd-op spinlock + 2 atomics on ONE
   per-Proc lock (stratumd's N I/O threads convoy). An RCU/seqlock
   fdtable must be re-audited against the #844 closed list.
7. **R2-F2/F3/F6 — the coarse-global-lock set**: torpor_lock (one lock,
   all futex traffic, held across the on_cpu spin — per-bucket split does
   not collide with I-9); g_proc_table_lock (held across the
   group-terminate wake-storm + broadcast IPI; narrowing is audit-bearing
   vs I-24/RW-2-F75); the TTAS spinlock fairness gap under 8-CPU
   contention (qspinlock-class fix; shrink the regions first).
8. **R1-F7 — SLUB per-CPU fast path** (the ARCH §6.4-committed mechanism,
   absent as-built — scripture honesty-marked in-arc, §6.1) + **R1-F5/F6**
   (magazines cover 2 of 19 orders; refill takes the zone lock per page —
   a `buddy_alloc_bulk` restores the intended Bonwick amortization).
9. **R2-F5 / R4-F7 — eager FP/SIMD** (1 KiB + 2 ISB per switch, FP-less
   kthreads included; lazy-FP is the documented seam; the
   conditional-FPCR-ISB half is trivial) + **R2-F7** (O(N) run-queue
   insert on every yield — folds into the EEVDF rbtree) + **R2-F4**
   (1 kHz always-on tick; NO_HZ_IDLE-class) + **R2-F8** (broadcast
   group-terminate IPI → targeted kick; needs a per-Thread cpu stamp).
10. **R4-F4 — spawn/exec eager-everything**: two full byte-copies of the
    binary (blob→kmalloc, then KP_ZERO-then-overwrite per segment) +
    eager 256 KiB stack zero; ut (388 KiB) ≈ 400–520 µs ≈ 2–2.5× the
    200 µs p50 — the session-start binaries are the ones that miss.
    Blob-backed RX Burrows (I-12-compatible) delete both copies; F3's
    block-copy is the 8–16× stopgap. (Subsumes R1-F10.)

### WATCH (fine at v1.0 scale; the breaking scale is named in §3 / the reports)

R1-F4 (demand-page TLBI on invalid→valid — **audit-bearing to optimize**,
the R7 F129 conservative choice is the correct v1.0 default); R1-F5
(global zone lock); R1-F8 (SLUB KP_ZERO byte-loop + per-iteration bound
reload, codegen-confirmed); R1-F9 (O(N) VMA list at musl-daemon N≈50–100);
R3-F1 (the 9P RTT ladder: 2 switches + 5 copies + 1 alloc + ~6 locks —
small ops fit, full-4 KiB strains); R3-F6 (pipe 4-copies/pair — F5 is the
fix; the structure is sound); R4-F6 (the notes peek takes `q->lock` on
EVERY sync return, no pending-flag short-circuit — a shared-cacheline RMW
across all threads of a Proc; a relaxed pre-check preserves I-19); R4-F8
(IRQ→EL0 ladder 0.6–1.4 µs reasoned — p99 fits, p50 at the line on GICv2
whose 4×`dsb sy`/delivery is the audited HVF price); R4-F9 (per-char
polled UART TX — invisible under QEMU, ~3.5 s of pure TX for the test
boot's ~40 KB at a real 115200 serial; no post-boot hot path prints —
verified); R4-F11 (the projected 9P spawn leg, pairs with #58).

## 5. Verified in-budget (checked and judged fine — do not re-derive)

- **Syscall entry/exit floor**: ~130–160 instr, no barriers beyond
  svc/eret, dense jump-table dispatch, lockless die-check (2 ldar,
  cheap-guard-first) → reasoned 100–160 ns; **measured 83 ns/op** (HVF
  loom NOP, n=64 amortized, boot-invariant ×3). The <200 ns row is MET —
  Linux-class for a full-save, uniform-EL1h (I-21), zero-leak kernel
  (seL4's 30–50 ns fastpath is a different contract). [R4-F5 + SA]
- **The audited mechanisms are cheap or SOTA-matching**: #713 daifset
  windows + die-check placements ≈ 1 instr each ("the audited soundness
  is performance-free" — R4); the #844 32-byte snapshot is intrinsic +
  cheap (the lock is the cost, R3-F7); the #841 elected reader IS the
  Plan 9 mountio / Linux v9fs single-demuxer shape (R3-F8); SQPOLL parks
  on a rendez — zero idle burn, matches-or-beats io_uring's
  spin-then-park (R3-F10); the ASID fast path is ONE lock-free CAS, no
  per-switch TLBI (the rolling-ASID design already deleted the classic
  cost) [R2/R4].
- **pick_next O(1)**; per-CPU run queues with trylock-backoff steal (no
  global rq lock); wakeup's lock discipline releases `g_timerwait.lock`
  before the on_cpu spin [R2, R3].
- **alloc_pages KP_ZERO**: outside the buddy lock, `stp xzr,xzr`-widened
  (codegen-confirmed); buddy ops O(1) with lock scope = list ops only;
  magazine fast path genuinely per-CPU lock-free; pgtable-walk table
  allocs amortize to ~0 [R1].
- **Boot mechanics**: BSS clear sub-ms; KASLR's 1982 relocs ≈ µs;
  struct-page + direct-map init ≈ 10–25 ms reasoned; PSCI bring-up
  ~ms/CPU. The boot-budget eater is the build shape (R4-F1/F2), not
  initialization [R4-F10].
- **devramfs O(98) linear lookup**: negligible vs ELF load on the spawn
  path; not worth indexing [R3-F11].
- **9P small-op latency**: present-proxy full-path round-trip measured
  24.4–60.6 µs (floor ~24 µs) on HVF — the <50 µs p50 row is plausibly
  MET for the metadata-class ops that dominate interactive use [SA].
- **argon2id placement**: per-login KDF (~50 ms-class at t=2/16 MiB)
  runs after credential entry — correctly NEVER in the boot-to-prompt
  window; corvus's boot self-test is deliberately argon2-free [R4-F2].

## 6. Measurement-infrastructure gaps (the G-list)

- **G1** (elevated to V1.0-RISK as R4-F1/F2): no production boot
  configuration — `test_run_all()` is unconditional (main.c:654; every
  test_*.c in the unconditional kernel sources) and joey's probe ladder
  (USER_CREATE/RECOVER/login E2Es, torture + bench probes) rides every
  boot, so the two boot-budget rows cannot be measured as committed.
  VISION:171's "9P round-trip: gated at Phase 4 exit" was likewise never
  numerically gated.
- **G2**: no pipe/kernel-handled-syscall bench; VISION:170's "Syscall
  latencies: measured continuously from Phase 2 onward" was a fossil — no
  such bench ever existed (honesty-fixed in-arc, §6.1). The loom NOP is
  the nearest proxy.
- **G3**: no spawn/exec bench (the 200 µs row); R4's reasoned
  decomposition + the boot-log envelope are the only data. A per-stage
  CNTVCT delta in joey would firm F2 cheaply (R4).
- **G4**: irq-bench measures the test-mode slice-wait path (SA-1a) — it
  runs before `sched_set_notify_enabled(true)` AND before the secondaries
  arm their ticks, so the production wake paths (notify-IPI steal;
  1 ms tick-steal backstop) have never been measured by anything. Variant
  candidates: run post-enable (joey-spawned like loom-bench), park the
  driver in tsleep (idle-CPU0 wake), or both. Each measures a DIFFERENT
  real path; the bench must state which it measures (41-irq-bench.md
  honesty-noted in-arc, §6.1).

### 6.1 In-arc trivially-mechanical fixes (the only edits that landed)

Per the standing policy (non-soundness X-lens items may ride the report
commit when trivially mechanical, non-ABI):

1. `kernel/sched.c` `sched_notify_idle_peer` comment — "secondaries have
   no per-CPU timer at v1.0" was false in production (smp.c arms banked
   timers at `smp_enable_secondary_preemption`); rewritten to the
   quiescent-phase truth.
2. `mm/magazines.c` header — claimed "each buddy-lock acquisition covers
   MAGAZINE_SIZE pages"; the code takes the lock per page on refill/drain
   (R1-F6). Header now states the as-built behavior + names the bulk-op
   lift.
3. `arch/arm64/fault.c` demand-page comment — claimed the EL0 branch is
   "dead code in production" and that faults extinct at v1.0; both
   superseded (live login-shell path; proc_fault_terminate + snare:*).
4. `docs/ARCHITECTURE.md §6.4` — the "per-CPU active slab… without lock"
   + `slub_debug=1` mechanism bullets marked DEFERRED (design-time
   fiction vs the as-built global-locked slab; R1-F7) — the RW-10
   fossil class.
5. `docs/VISION.md §4.5` gating notes — "measured continuously from
   Phase 2 onward" + "gated at Phase 4 exit" replaced with the honest
   measurement status (G1–G4 pointers).
6. `docs/reference/41-irq-bench.md` — a "What this bench actually
   measures" correction (SA-1a): the documented numbers are slice-quantum
   readings under the test-mode quiescent scheduler, not IRQ-path
   readings.

## 7. Verification

- **Empirical method**: 3 timestamped HVF boots + 1 TCG boot of the dev
  kernel at `85f1868` (per-line wall-clock timestamper over
  `run-vm.sh --no-share --cpus 4`); logs `/tmp/rw11-boot-*.log`
  (session-local). Numbers quoted in §1 are boot-invariant where claimed
  (loom NOP 83 ns ×3; irq-bench 6.022–6.024 ms ×3); load-sensitive where
  marked (present-proxy 24–61 µs). The host concurrently ran the reviewer
  agents (API-bound); QEMU numbers are substrate numbers — bare-metal
  measurement remains future work (G-list).
- **Causal pins verified in source by the main loop** (not taken from
  reviewers): `THREAD_DEFAULT_SLICE_TICKS=6` (sched.h:48);
  `sched_set_notify_enabled(true)` at main.c:734 AFTER `test_run_all()`
  at :654; `ready_on`'s same-CPU no-need_resched branch (sched.c:667-681);
  secondaries' timers armed only at `smp_enable_secondary_preemption`
  (smp.c:488-495); `SYS_ATTACH_DEFAULT_MSIZE=4096` (syscall.c:1033) +
  `SRVCONN_RING_CAP=8192` (srvconn.h:79); `uaccess_load_u8` as the sole
  primitive + the per-byte loops (syscall.c:811/839); KP_ZERO outside the
  buddy lock (phys.c:287-316); per-CPU magazines fronting orders {0,9}
  only; `order_for_pages` power-of-two rounding (burrow.c:88); SLUB
  per-alloc `c->lock` (slub.c:232); the unconditional test build
  (CMakeLists + main.c:654); the in-tree `boot-time:` self-measure lines.
- **Reviewers**: 4 × `holotype-reviewer`, all `MODEL(start)==MODEL(end) ==
  claude-fable-5` (no fallback). Spot-verified quotes: context.S FP
  save + unconditional ISB; spinlock.h TTAS; insert_sorted list walk;
  ARCH §6.4 text; fault.c/magazines.c comments.
- **Soundness**: **0 SOUNDNESS findings** from all five prosecutors — the
  audited protocols (on_cpu, death-wake, register-then-observe, #841,
  #844, #845, Loom I-29/I-30, ASID I-31) held under the perf-lens trace.
  One premise correction (R4: no production build exists — my reviewer
  prompt assumed otherwise); adjudication deltas recorded in §4 (R2's
  WATCH vs the V1.0-RISK budget-gate rating).
- **Post-edit posture**: kernel rebuilt clean; `tools/test.sh` PASS
  (825/825 + boot OK + 0 EXTINCTION) after the comment-only kernel edits
  — see the close commit. No SMP gate run (no kernel behavior changed:
  comments + docs only).
