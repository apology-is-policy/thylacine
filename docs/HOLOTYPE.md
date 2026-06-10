# HOLOTYPE — the whole-system deep review arc

**Status**: ACTIVE. DFS-inserted 2026-06-10 (user-approved), pre-empting the
LS emerge at the post-LS-5c boundary (`9886704`). The RW status table is at
the bottom of this document; per-RW detail lands in `docs/holotype/NN-*.md`.

---

## 1. What this is, and why now

In taxonomy, the **holotype** is the single physical specimen to which a
species' name is formally attached — the reference against which every later
identification is judged. This arc examines our specimen end to end and
writes its authoritative description: every deviation from soundness, from
completeness, from the state of the art, from our own scripture, and from
what real workloads will demand — found, documented, registered, and either
fixed in-arc or roadmapped deliberately. The deliverable is the **register
and the re-planned roadmap**, not a rewrite.

Why now, in priority order:

1. **The system just crossed the real-interactive-OS line** (login → shell →
   coreutils → Ctrl-C). This is exactly when as-built coherence reviews pay
   off: before Phase-7 hardening/fuzzing bakes the current shapes in, and
   while a defect register is cheaper than fuzz findings.
2. **The deep-smp-review precedent.** One focused deep review (2026-06-05)
   found `specs/scheduler.tla` structurally blind to the #860 bug class,
   retired the #788/#806/#860 point-patch pile, and replaced it with a sound
   design. Holotype is that, system-wide.
3. **The oldest surfaces were audited by older models and have the most
   churn layered on top.** Memory, boot, and exception entry date to Phases
   1–3; dozens of point fixes (#713, #807, #808, #847, F3/F4/F5...) have
   accreted since. They are the highest-marginal-value re-review
   targets for the current model.
4. **The model window.** The reviewing model (Fable) is available for a
   bounded number of days. The RW order below is a priority order: if the
   window closes early, the highest-risk surfaces are already done, and
   RW-13-lite (consolidate what exists) can run at any boundary.

Standing rules inherited unchanged: whole-system stewardship (encounter a
bug → enqueue a bug, before anything else), the elusive-bug discipline,
ground truth over theory, the audit-in-flight parallel-work discipline,
re-audit on dirty close, plain-ASCII commits, user pushes.

---

## 2. The six lenses

| Lens | Symbol | Question | Severity scale |
|---|---|---|---|
| Soundness | S | Is it correct under real-world conditions — concurrency races, object lifetimes, permission boundaries, memory safety, malformed-input handling? | P0–P3 (the existing severity scale) |
| Completeness | C | Is anything stubbed, silently omitted, deferred-forever, or missing its error paths — does the system actually do what scripture says it does? | P0–P3 where it is a live defect; H1–H4 where it is owed work |
| SOTA | T | We built this mechanism this way; is there a better-known shape — or one more in line with the rest of the system? | H1–H4 |
| Performance | P | Will real workloads perform sub-par vs SOTA OSs here? What are the alternatives and how do they fit scripture? | H1–H4 |
| Consistency | X | Are components consistent with each other, the philosophy, and the scripture (the 9P principle, per-Proc namespace, two-axis authority, fd-first)? | H1–H4 |
| Gaps | G | What did we miss — in scripture or implementation — that real OSs have and real applications/workloads will expose (e.g. a packet filter)? | H1–H4 |

**H-scale** (non-soundness impact): **H1** = v1.0-blocking (ship-stopper for
the v1.0 contract; escalate to the user immediately — it may re-plan the
arc). **H2** = should land before v1.0-rc. **H3** = v1.x roadmap item.
**H4** = note/record only (documented, no work owed).

**Named exemptions** (user-set, 2026-06-10): the Consistency lens exempts
performance paths (the Loom ring transport) and compat paths (the pouch
byte-mode socket transport). They are deliberate, documented departures.

---

## 3. Structure: two methods, not 6 × 17

Running all six lenses over every subsystem is combinatorial waste. Instead:

- **Per-area deep reviews** (RW-1 … RW-9) carry three lenses each:
  **S**oundness (rigorous correctness review, the existing review discipline,
  cumulative closed lists as do-not-re-report preamble), **C**ompleteness,
  and **area-local SOTA** (with the prior-art rule: Plan 9 idiom first, then
  Fuchsia/Genode/seL4; for performance *techniques* Linux is admissible even
  though its ambient-authority model is not).
- **Three global cross-cuts** (RW-10 … RW-12) carry the inherently
  relational lenses over the whole system at once: Consistency (+ the
  invariant ledger + spec/doc drift), Performance, Gaps.
- A per-area review that trips over a P/X/G observation records it in its
  report anyway (tagged with the lens); the cross-cut aggregates them. No
  finding is dropped because it arrived under the "wrong" RW.

---

## 4. Depth tiers

| Tier | Meaning | Assigned to |
|---|---|---|
| **FULL** | Read every line; exhaustive correctness review; assume the prior review missed things | RW-1 (memory), RW-3 (exception entry + syscall surface), the death-path/notes/wait-wake half of RW-2, the corvus half of RW-6 |
| **STANDARD** | Read fully; review the load-bearing paths in depth; sample the rest | RW-4, RW-5, RW-7, RW-8, RW-9, the cross-cuts |
| **DELTA** | Converged recent reviews: re-examine only what changed since their close + their interaction with neighbors | The sched-redesign core (sched_alpha, #863/#864) in RW-2; Loom (6 closed rounds + 3 spec modules) in RW-4; the #844 handle-lifetime pass in RW-5; the A-5c recovery crypto in RW-6 |

Stratum: only the Thylacine-facing seam is in scope (the `bdev_thylacine`
arm, the 9P server contract as we consume it, the `/ctl` DEK surface) — it
shipped v2 under its own audit culture. A Stratum-side defect found through
the seam is still OURS (stewardship): enqueue + fix per the 2026-05-29
authorization.

---

## 5. Finding lifecycle

- **IDs**: per-report sequential `F<n>` (the audit convention), globally
  qualified as `HT<NN>.F<n>` (NN = the report number) in the register. Every
  finding carries (area, lens, severity, file:line, reasoning chain or
  rationale, disposition).
- **The register**: `docs/holotype/00-register.md` — one row per finding,
  appended per RW, dispositioned at RW-13. The register is the arc's primary
  artifact.
- **Enqueue rule**: every actionable finding gets a `TaskCreate` at the
  moment it is confirmed — the register row alone is prose, and prose is a
  bug being walked past in slow motion.
- **Fix policy** (user-set, 2026-06-10): soundness **P0, P1, and P2 are
  fixed in-arc**, each with a regression test (the test fails pre-fix) and
  the dirty-close re-audit rule applied to invasive fixes. P3s are tracked
  or closed-with-justification. Non-soundness findings (T/P/X/G) are
  **registered, not fixed in-arc** — they are design-debt and take the
  scripture-first path as future chunks — EXCEPT trivially-mechanical,
  non-ABI items (a wrong comment, a missing assert, dead code), which may
  ride the RW's report commit.
- **Regression rule**: every fixed soundness finding that can be made to
  fail lands a failing-pre-fix test. Every Gap finding names the **workload**
  that exposes it (not just the missing feature).
- **Closed lists**: every RW's reviewer prompt includes the relevant
  `memory/audit_*_closed_list.md` files as do-not-re-report preamble; every
  RW close appends its own (`memory/audit_holotype_rwN_closed_list.md`, or
  the standard per-surface name where one exists — RW-0 appends
  `audit_ls5_closed_list.md`).

---

## 6. The operating loop per RW

1. **Ground**: read the area's scripture (ARCH sections + design docs) +
   `docs/reference/NN-*.md` + the mapped closed lists. List the file scope.
2. **Spawn** the reviewer agent(s) in the background (general-purpose,
   most capable model, `run_in_background`) with the CLAUDE.md template
   extended by the three per-area lenses (S + C + local-T) and the area's
   focus list. Multiple agents per RW where the file scope is large
   (split by sub-surface, never by lens).
3. **Parallel** (the audit-in-flight discipline): first non-colliding work
   (report scaffolding, register upkeep, the next RW's grounding), then a
   same-surface self-review. Two reviewers bias differently; merge
   findings with equal authority.
4. **Validate**: trust-but-verify every quoted file:line before acting.
5. **Fix** P0/P1/P2 soundness in-arc (+ tests, + matrix, + dirty-close
   recursion); register + enqueue everything else.
6. **Land**: `docs/holotype/NN-<area>.md` (findings, dispositions, the
   verified-sound list — what was reviewed and survived, so the next
   reader knows what was checked) + the register append + the status row
   below + the closed-list append. One report commit per RW; fix commits
   separate.

---

## 7. The roadmap

Priority-ordered: the order IS the model-window contingency.

| RW | Scope | Tier | Key inputs |
|---|---|---|---|
| **RW-0** | The owed LS-5 focused audit (#963): the 3 LS-5 commits (`2a6c9ed` + `9f407d0` + `9886704`) + `tools/interactive/ls-5.exp` + full matrix + SMP gate. Doubles as the methodology calibration run. | FULL | ARCH 8.8.1/8.8.2, LIFE-SUPPORT LS-5, closed lists: 926_u6f, u7pre, 841/845, ls4 |
| **RW-1** | Memory: buddy, slub, magazines, vm/vma, burrow, mmu (incl. user-PTE uninstall, demote, patcher W^X), fault/demand-page, wxe, phys | FULL | ARCH §6/§28; closed lists: p6 memory hazards, 847, w15 |
| **RW-2** | Sched/SMP/threads/Procs: EEVDF + on_cpu + HMP (DELTA), smp/IPI, timer; death paths (exits/group-terminate/#811/#926/wait_pid v2), notes, rendez/sleep/tsleep, torpor, poll, pipe | FULL (death/wait-wake) + DELTA (sched core) | ARCH §7/§8; sched_alpha/oncpu specs; closed lists: ls5 (from RW-0), 926_u6f, u7pre, 809/811-era |
| **RW-3** | The privilege chokepoint: exception entry (vectors/start/userland/context trampolines, #713 eret windows), syscall dispatch + EVERY handler's arg validation + uaccess + the ABI coherence sweep (numbering, errno, arg conventions, rights gates) | FULL | ARCH §25.4 entry rows; ERRORS.md; the #713 case |
| **RW-4** | Namespace/FS: territory, stalk (I-28), Spoor lifecycle, Dev vtable + every dev* driver, the 9P client stack (client/session/transports/attach), Loom (DELTA), the Stratum seam | STANDARD (+DELTA Loom) | STALK-DESIGN, ARCH §9/§21, LOOM.md; closed lists: stalk1-3c, 841, 845, loom, 955, 957 |
| **RW-5** | Authority: handle table (#844 DELTA), caps/clearances/legate, perm.c rwx, identity plumbing, devcap/devproc gates | STANDARD | IDENTITY-DESIGN, IMPERIUM-DESIGN (read-only context); closed lists: 844, a4c1/2, 828, a5c |
| **RW-6** | Services: corvus (FULL — crypto: wraps, AEGIS, argon2id, BIP-39, session ownership, rate limits; A-5c core DELTA), joey (init semantics, reaping, bringup ordering), login (secret hygiene, getty) | FULL (corvus) + STANDARD | CORVUS-DESIGN; closed lists: 828, a5c, a1b |
| **RW-7** | Drivers/HW: gic (v2+v3), uart/cons RX, virtio family (blk/rng/gpu/input), kobj mmio/irq/dma, random/chacha, Halls of Extinction | STANDARD | PORTABILITY.md; closed lists: w2, w3, a4c1 |
| **RW-8** | Userspace runtime: libthyla-rs (alloc, fs, process, io, notes, torpor, loom, 9p) + the pouch boundary-line patches + the stratumd arm | STANDARD | ARCH §3.5; pouch patch headers; closed lists: 16b/c-era |
| **RW-9** | The Utopia stack: ut + libutopia (tokenizer/parser/eval/jobs/notes/line-editor/REPL) + coreutils sample | STANDARD | UTOPIA-SHELL-DESIGN; U-arc status rows |
| **RW-10** | **Cross-cut: Consistency** — the scripture-derived rubric (9P/file-first; per-Proc namespace; two-axis authority; fd-first notes; no ambient authority; naming/style; doc-per-chunk currency); the **invariant ledger** (every ARCH §28 invariant × enforced-where / tested-where / doc-current / residuals); spec-vs-code drift (SPEC-TO-CODE.md) | STANDARD | ARCH §28, all reference docs, specs/ |
| **RW-11** | **Cross-cut: Performance** — measure against the VISION latency budget where benches exist (irq-bench, loom-bench); inventory lock granularity (the global torpor_lock, g_proc_table_lock breadth, the per-9P-session serialization + single elected reader, the per-Proc handle-table lock, buddy zone lock, per-page TLBI on unmap, the 1000 Hz tick, KP_ZERO-everywhere); for each hot-path delta vs SOTA, name the alternative AND its scripture fit | STANDARD | VISION.md latency budget; ARCH §8/§21 |
| **RW-12** | **Cross-cut: Gaps** — workload-driven, not feature-list-driven. Canonical workloads: W1 multi-client file service; W2 a build/compile storm (spawn/pipes/fs-metadata churn); W3 multi-user interactive sessions; W4 a long-running network service (feeds the Phase-7 net design: sockets, packet filter, observability); W5 container-style namespace assembly + resource limits; W6 an editor/TUI (termios/PTY depth). For each: what breaks or is missing, in scripture or implementation | STANDARD | COMPARISON.md, NOVEL.md, `usr/apps/DOC-GAP-REPORT.md` (the aux track's input), ROADMAP Phase 7 |
| **RW-13** | **Consolidation**: triage every register row (v1.0-pull / v1.x / NOVEL-candidate / rejected-with-rationale), re-plan the forward roadmap (where LS-6/7/8/K re-enter, what got inserted), decide the emerge point. Lands as a scripture commit with user signoff. | — | The full register |

RW-0 deliverable is the standard audit close (+ `audit_ls5_closed_list.md`);
holotype reports start at RW-1 (`docs/holotype/01-memory.md`, numbered by RW).

---

## 8. Cross-cut charters (seeds, not bounds)

**RW-10 Consistency** starts from these rubric questions and extends them:
which syscall-shaped surfaces SHOULD be files per the Plan 9 principle and
are not (and is the deviation argued anywhere)? Is the error convention
coherent at each boundary (flat `-1` vs `-errno` vs ERRORS.md's staged
rollout)? Are ABI `_Static_assert`s present everywhere scripture demands?
Does every landed chunk have its reference-doc section (the doc-per-PR
rule), and which sections have drifted from as-built? Which `specs/*.tla`
are stale relative to the code they pin, and is `SPEC-TO-CODE.md` honest
about it? The invariant ledger is the RW's table deliverable.

**RW-11 Performance** is an *inventory + comparison*, not an optimization
pass: no perf rework lands in-arc. Each finding names (mechanism, measured
or reasoned cost, the SOTA alternative, scripture fit, suggested tier).

**RW-12 Gaps** must distinguish three kinds: scripture gaps (the design
never considered it), implementation gaps (scripture has it, the tree does
not), and seam gaps (deliberately deferred but with no recorded re-entry
point). Only the third kind is "fine as-is" — and only if the seam gets its
re-entry recorded.

---

## 9. Contingency + emerge

The RW order is the contingency plan: any boundary is a valid stopping
point, and RW-13-lite (consolidate whatever exists) can run early if the
model window closes. The LS arc resumes at the RW-13 emerge decision —
LS-6 (#949) / LS-7 (#950) / LS-K (#951) / LS-8 (#952) / LS-test (#953) /
Imperium (#959), re-sequenced by whatever Holotype found.

---

## 10. Status

| RW | Scope | Status |
|---|---|---|
| RW-0 | LS-5 audit fold-in (#963) | **done** — 1 P1 (ut eager note-queue open, `f145ce8`) + 3 P3 (F3 kproc latch guard fixed; F2/F4 accepted + doc); `ls-5.exp` landed; register seeded (HT00.F1–F4) |
| RW-1 | Memory | pending |
| RW-2 | Sched/SMP/threads/death/wait-wake | pending |
| RW-3 | Exception entry + syscall surface | pending |
| RW-4 | Namespace/FS + 9P + Loom(DELTA) | pending |
| RW-5 | Handles/caps/identity/perm | pending |
| RW-6 | corvus/joey/login | pending |
| RW-7 | Drivers/HW + HX | pending |
| RW-8 | libthyla-rs + pouch | pending |
| RW-9 | Utopia stack | pending |
| RW-10 | Cross-cut: consistency + invariant ledger | pending |
| RW-11 | Cross-cut: performance | pending |
| RW-12 | Cross-cut: gaps | pending |
| RW-13 | Consolidation + emerge (scripture commit) | pending |