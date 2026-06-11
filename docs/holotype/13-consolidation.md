# HOLOTYPE RW-13 — Consolidation + emerge

**Status**: the arc capstone. Lands as the scripture commit that closes the
HOLOTYPE arc (RW-0..RW-12 all CLOSED) and re-plans the forward roadmap.
**Decisions voted**: 2026-06-11 (the four scope forks in section 2).
**Inputs**: the full register (`docs/holotype/00-register.md`, HT00..HT12),
the twelve per-RW reports, the closed lists, the open task set (#7..#70).

This document is the authoritative answer to "what did the thirteen-part
deep review conclude, and what is the plan from here." It does not rewrite
the system; it records the specimen's description and the re-planned road.

---

## 1. What the arc found

**Zero P0 anywhere.** Across nine per-area deep reviews (RW-0..RW-9) and
three global cross-cuts (RW-10..RW-12), the holotype found no actively-broken
correctness/security/safety violation in the whole tree. Every latent P1 and
every P2 was fixed in-arc with a regression. The three cross-cuts each
returned **zero soundness findings** — the system is correct; what drifted is
the scripture *describing* it and the userland *frontier ahead* of it.

### 1.1 Soundness scorecard (per round)

| RW | Surface | P0 | P1 | P2 | Disposition |
|---|---|---|---|---|---|
| RW-0 | LS-5 interactive Ctrl-C | 0 | 1 | 0 | fixed (`f145ce8`) |
| RW-1 | Memory | 0 | 2 | 3 | fixed (ASID rollover + multi-thread-fault) |
| RW-2 | Sched/SMP/death/wait-wake | 0 | 4 | 1 | fixed (`e504e8b`/`0ce638b`) |
| RW-3 | Exception entry + syscall | 0 | 0 | 4 | fixed (`01e1b40`) |
| RW-4 | Namespace/FS + 9P + Loom | 0 | 2 | 3 | fixed (`ee30f55`/`6cf5933`); dirty-close r2 |
| RW-5 | Authority (handles/caps/perm) | 0 | 0 | 1 | fixed |
| RW-6 | corvus/joey/login | 0 | 0 | 5 | 4 fixed + 1 deferred (#876) |
| RW-7 | Drivers/HW + Halls | 0 | 4 | 4 | fixed; dirty-close r2+r3 |
| RW-8 | libthyla-rs + pouch | 0 | 1 | 7 | fixed (`cc79551`); dirty-close r2 |
| RW-9 | Utopia stack | 0 | 1 | 12 | fixed; dirty-close converged r3 |
| RW-10 | Cross-cut: Consistency | 0 | 0 | 0 | 0 soundness; 15 X fixed/registered |
| RW-11 | Cross-cut: Performance | 0 | 0 | 0 | 0 soundness; 2 V1.0-RISK + backlog |
| RW-12 | Cross-cut: Gaps | 0 | 0 | 0 | 0 H1; 60 gap findings registered |
| **Total** | | **0** | **~15** | **~40** | **all fixed in-arc** |

### 1.2 The single most important systemic finding

**The P6 multi-thread-Proc lift outran the wait/wake machinery and the
per-Proc-shared-state serialization — and almost every P1 in the arc lived
exactly there.** RW-2 (poll-waiter / wait_pid single-waiter), RW-4 (the
unlocked per-Territory mount table), RW-5 (the non-atomic `p->caps` writer),
RW-7 (the single-waiter IRQ rendez + the cons->notes->SAK seam), RW-8 (the
NDFLT death-arm), RW-9 (the eval/notes/job seam) — six rounds, the same
class. Each layer was *correct when it landed*; then `#809`/`#811` (group
terminate + universal death-interruptible sleep), A-3 (identity), stalk
(per-Territory `/srv`), and LS-4 (per-Proc cwd) moved the substrate under it,
and the single-waiter `Rendez` / lockless-shared-field / unlocked-RMW that
was safe for one thread became an unprivileged extinction or a UAF for two.
The scripture response landed in-arc: `DEBUGGING-PLAYBOOK.md` 6.15 (the
multi-thread-lift recurrent class) + the CLAUDE.md self-audit "multi-thread
per-Proc shared state" check ("no current program drives two threads in here"
is the latent-P1 trap, not a safety argument). This is the arc's durable
lesson and the standing hazard for every future per-Proc surface.

### 1.3 The three debts (what the cross-cuts surfaced instead of bugs)

1. **Reconciliation debt** — the scripture's phase records and present-tense
   capability claims drifted out of agreement with each other and the tree,
   because every re-scope (the LS arc, the convergence detours, the Pouch
   renumber) updated the new home and left the old claim standing. The tell
   is RW-10's fossil class one level up: not "a spec name is stale" but "a
   *phase assignment* is stale in three docs and the invariant that depends
   on it points at the wrong chunk." RW-10 fixed the invariant ledger + spec
   fossils in-arc; RW-12 found the residue is a scope decision (now voted) +
   a doc-hygiene execution (#64).
2. **A small pre-rc risk/floor set** — the 6 ms wake-preemption slice cliff
   (#60, the latency-budget gate cannot pass without it), no production boot
   configuration (#61), and the unbuilt Resource/DoS floor (#65, a fork/
   thread/memory bomb currently extincts the box). Real, scope-independent.
3. **The userland-completeness frontier** — the gaps are the *userland
   surface a real workload needs*: exec-from-namespace, a wall clock,
   termios/PTY, an editor, the network stack, an on-system toolchain. Almost
   all were already on the LS / Phase-8 / Phase-9 horizon; section 2 sets
   where the v1.0 line falls through them.

---

## 2. The four scope decisions (voted 2026-06-11)

The gaps round forces the question RW-13 exists to answer: **where does the
v1.0 line fall through the userland frontier?** The user voted the maximal
coherent v1.0 — a textual multi-user interactive OS with containers,
networking, an on-system toolchain, and an editor. Recorded:

| # | Fork | Decision | Consequence |
|---|---|---|---|
| **D1** | Networking in v1.0? | **In v1.0** (Phase 8) | The net stack stays in the v1.0 contract. Shape decided by lineage: Plan 9 `/net` via a `netd` userspace 9P server (smoltcp under it, Loom-amortized), composing with per-Proc territory. #68 fills the 7 design holes inside ROADMAP section 9.1. |
| **D2** | Containers in v1.0? | **Build the tractable core** | exec-from-namespace (#58 — route the 5 spawn variants through stalk + X-search; a contained fix since the territory primitive already exists) + the ns introspection substrate (#66 — Spoor name-retention so the Plan 9 `ns` tool renders). Union mounts flip COMPARISON ✓ -> ○ to v1.x with a fail-loud API. |
| **D3** | On-system toolchain? | **In v1.0** | clang/lld + make + git ported via Pouch become a Phase-8 deliverable. The self-hosting / build-storm (W2) story is part of v1.0. VISION section 397 "no subset" + the Phase-9 "parallel make" criterion are now *correct* and get a phase home. (Scope weight: this is the single largest Phase-8 pole; recorded honestly per "system over cost/scope.") |
| **D4** | First work after RW-13? | **Pre-rc hardening sub-arc** | #60 (wake-cliff) + #61 (boot-shape) + #65 (resource floor) land first — scope-independent soundness/risk, not optional. |

Two decisions remain owed and are tracked separately (not capstone-level):
**#20** (the retrospective `-T_E_*` errno rollout — a standing user vote since
RW-3) and the spec-first re-enablement posture (status quo: per-surface
suspension holds; `death_wake.tla` already landed for the death cascade).

---

## 3. The register triage

The ~250 register rows collapse: the overwhelming majority are already
dispositioned (FIXED in-arc, ACCEPTED-with-caveat, or v1.x-TRACKED). The
open set RW-13 places:

### 3.1 v1.0-pull (lands before the Phase-9 rc gate)

- **Pre-rc, scope-independent** (the D4 sub-arc, first): **#60** wake-cliff
  (additive check-preempt-on-wake; the ARCH section 8.2 EEVDF reconcile —
  2A-F6 — rides it), **#61** production boot shape (`KERNEL_TESTS=OFF` +
  gated joey ladder), **#65** Resource/DoS floor (per-Proc page/thread/child
  caps; the v1.x quota seam depends on its counters).
- **Container keystone** (D2): **#58** exec-from-namespace, **#66** ns
  introspection substrate, **#57** namespace-layout (bind /dev + /proc + /ctl
  per ARCH section 9.4). Pulled forward — exec-from-namespace is foundational
  for both the LS namespace tools and the Phase-8 container runner.
- **Reconciliation** (lands as the #64 commit, the first emerge chunk):
  the phase-record renumber propagation (section 2.1 is authoritative),
  the present-tense honesty pass, the I-20 PTY repoint, the nine-specs fossil.

### 3.2 The LS line, re-sequenced (Phase-7 / Utopia completion)

The interactive-OS milestone finishes here, re-ordered by what Holotype
found and by the editor's dependency chain:

- **LS-K (#951)** — id/whoami/date: self-identity (getpid/getuid) + the
  wall clock (clock_gettime + the RTC epoch via PL031). On the critical path
  (RW-11/RW-12 both flagged the missing clock). Absorbs #69's RTC-epoch and
  principal->name sub-seams.
- **LS-8 (#952)** — U-PTY: pollable cons + termios/consctl + the shell poll
  loop. The editor's prerequisite; repoints I-20. Absorbs #69's winsize-query
  and pouch-termios-patch sub-seams + the child-stdin wiring (W3-F6).
- **LS-7 (#950)** — nora, the native editor. Depends on LS-8 (termios) +
  child-stdin.
- **LS-6 (#949)** — login echo; folds into LS-8b's real ECHO.

### 3.3 Phase 8 (Linux compat + network + containers + toolchain)

- **The net arc (#68)** — section 9.1 net stack design-complete (fill the 7
  holes: packet filter, DNS/`/net/cs` mechanism, `/ctl/net` observability,
  poll-over-9p readiness, IP config, server-side W4 exit criteria, the
  section 9.3 spec-waiver re-scope) -> netd -> socket shim -> the W4 workload.
- **The container runner (#70)** — thylacine-run/OCI + the synthetic
  `/dev`-`/proc`-`/sys` Linux servers, on the exec-from-namespace + net-
  isolation foundations. Re-anchored from the stale section-9 "Phase 6" label.
- **The on-system toolchain (#67, NEW v1.0 scope from D3)** — clang/lld +
  make + git via Pouch.
- **The Linux ARM64 binary shim** (the existing section 9.1 deliverable).

### 3.4 Phase 9 (Hardening -> v1.0-rc) and beyond

- The **#62** perf backlog items that gate the latency budget (top: per-byte
  uaccess -> block copy_*_user) + fuzzing + 8-CPU stress + the budget gate
  (gated on #60). -> v1.0-rc.1.
- **Phase 10**: Halcyon -> v1.0 final (the ROADMAP section 10/11 fallback
  structure stands: if Halcyon slips, the Phase-9 rc ships as v1.0).

### 3.5 v1.x backlog (deferred-with-recorded-re-entry)

The H3 per-RW follow-ups (#23, #26, #28, #46, #54), union mounts (D2),
the LS-9 cluster (ln/readlink/export/env/find-exec), the non-gating #62 perf
items, #20 (errno sweep, pending the separate vote), the H4 record-only set
(seccomp non-goal, mouse/OSC52 -> Halcyon, daemon-logging convention).

### 3.6 NOVEL-candidates (recorded, build deferred)

No *new* NOVEL angle emerged from the gaps round — the surfaced gaps are
completeness, not invention. The existing NOVEL.md angles (the 9P-server
library #1, `/ctl/9p` observability #3, Loom-rides-net #1) are reaffirmed as
Phase-8 deliverables, not re-opened.

---

## 4. The emerge point

**The HOLOTYPE arc ends at this commit.** The deep-review cadence
(RW-close -> next surface) is complete; the system resumes *building*, with
the Holotype findings folded into the roadmap at their triaged priority. The
forward sequence:

1. **This commit** — RW-13 capstone (decisions + triage + re-plan).
2. **#64** — the reconciliation pass (the first emerge chunk; pure scripture,
   no code): propagate section 2.1's canonical phase numbers into the
   dangling pointers, the present-tense honesty edits, the I-20 repoint, the
   nine-specs fossil. Directive in section 5.
3. **The D4 pre-rc sub-arc** — #60 + #61 + #65.
4. **The container keystone** — #58 + #66 (+ #57).
5. **The LS line** — LS-K -> LS-8 -> LS-7 -> LS-6 (Phase-7 completion).
6. **Phase 8** — net arc -> container runner -> toolchain -> Linux shim.
7. **Phase 9** — hardening -> v1.0-rc.1.
8. **Phase 10** — Halcyon -> v1.0 final.

---

## 5. The #64 reconciliation directive

The deep present-tense honesty + phase-renumber pass executes against an
*authoritative source*: ROADMAP section 2.1 is the canonical phase registry
(the long-deferred header renumber that RW-12 escalated). #64 propagates it:

- **Phase-number propagation**: the section-9 header ("Phase 6: Linux compat
  + network" -> Phase 8), section-10 ("Phase 7" -> Phase 9), section-11
  ("Phase 8" -> Phase 10), and the high-traffic in-body "Phase N" refs that
  the 5-way net naming flows from. Net = Phase 8; v1.0-rc = Phase 9.
- **Dangling pointers**: VISION:315 -> ARCH section 13 (now MMIO/VirtIO),
  ROADMAP:1450 "section 16 (TBD subsection)", the NOVEL/ARCH net-phase cites.
- **I-20 repoint**: ARCH section 28 I-20 says PTY "lands with LS-8" but LS-8
  excludes the master/slave mechanism -> repoint to the Phase-8/PTY chunk
  that owns master/slave; reconcile ARCH section 23.8's Phase-7 "must haves"
  (Helix + PTY + /tmp-tmpfs) to the LS / Phase-8 reality.
- **Present-tense capability honesty**: union mounts (COMPARISON ✓ -> ○,
  fail-loud the no-op `bind_before/after`), the per-Proc-9P-connection claim
  (ARCH:2171, descope to the shared-multiplexed reality), the advisory-lock /
  xattr / `/ctl/9p` present-tense claims (VISION section 328/333/397).
- **The nine-specs fossil**: COMPARISON:259/271/318 + NOVEL Angle #8 +
  estimate.md:41 -> the 17-module inventory (the 3 named — futex/notes/pty —
  were dropped 2026-05-23).
- **The D3 toolchain home**: record the on-system toolchain as a Phase-8
  deliverable in ROADMAP section 9.1 + VISION section 397 (now coherent).

Each edit verifies against as-built first (the doc-per-chunk discipline);
the reconciliation does not invent, it makes the records honest to the tree
and to the voted v1.0 line.

---

## 6. The arc in one line

The specimen is sound. The thirteen reviews confirmed zero P0 across the
whole system, fixed every latent defect, named the one systemic hazard (the
multi-thread-Proc lift), made the scripture honest to the tree, and re-planned
the road to a maximal-coherent v1.0: textual multi-user OS + containers +
networking + toolchain + editor, hardened to v1.0-rc at Phase 9, with Halcyon
the final phase. The thylacine is real; now it gets dressed for release.
