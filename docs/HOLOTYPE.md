# HOLOTYPE — the whole-system deep review arc

**Status**: COMPLETE (2026-06-11). DFS-inserted 2026-06-10 (user-approved),
pre-empting the LS emerge at the post-LS-5c boundary (`9886704`); RW-0..RW-13
all closed. The capstone is `docs/holotype/13-consolidation.md` (the
re-planned roadmap + register triage + the four voted v1.0 scope decisions);
the arc emerges at RW-13 and the system resumes building. The RW status table
is at the bottom of this document; per-RW detail lands in `docs/holotype/NN-*.md`.

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
| RW-1 | Memory | **CLOSED `@cb99514`** — 0 P0 across the surface; every P1/P2 fixed in-arc. A-F1/A-F2/A-F-S1 (3 P2) + B mmu 3 P3 + **C-F1 multi-thread-fault extinction (P1)** + C-F2/C-F3/C-F4 + B-F3/C-F6 doc-folds. **B-F1 ASID exhaustion (P1)** closed via the generation-rollover redesign: fail-soft `ef29456` → scripture `83bd74e` → spec `4fe50f7` (`specs/asid.tla`, model-first) → impl `d742ffa` → focused Fable audit `d40dbbb` (0 P0 / 1 P1 / 1 P2 / 2 P3; the P1 was a SPEC-fidelity defect, the kernel was always sound). 806/806; SMP gate 0 corruption. Catalog: `docs/holotype/{00-register,01-memory}.md` + `audit_holotype_rw1_closed_list.md`. Perf/SOTA → RW-11/RW-12; one tracked follow-up (DTB CPU-index map, F3). |
| RW-2 | Sched/SMP/threads/death/wait-wake | **CLOSED `@5033b94`** — 0 P0; **4 P1** all FIXED `@e504e8b` (2C-F1 poll-waiter-outlives-obj-ref UAF; 2A-F1 vd_t wake-no-rebase starvation; 2B-F1/F2 wait_pid single-waiter-extinction + lockless-cond UAF — all the multi-thread-Proc lift P6 landed but the wait/poll machinery never caught up to) + 5 P3 (sched/smp loud-fail + 4 doc) + 2 regressions. **1 P2 SURFACED** (2B-F3 orphan→kproc leak / ARCH §7.9 PID-1 divergence — init-reaping policy, user's call) → **NOW FIXED `@0ce638b`** (reparent-to-init + joey WNOHANG reaper + by-pid waits; Fable subagent impl + Opus eval + a formal Fable holotype-reviewer round, 0 P0/P1/P2, 4 P3). Dirty-close round-2 (2 Fable on the fixes): **clean** (0 P0/P1/P2; 3 P3 doc/test fixed). The #809/#811/LS-5/#926 death cascade is SOUND under composition. 808/808; SMP gate 0 corruption. Catalog: `docs/holotype/{00-register,02-sched-death-waitwake}.md` + `audit_holotype_rw2_closed_list.md`. REGISTERED → RW-13: 2A-F6 (not-real-EEVDF), SA-1 (no death-wake spec — user vote), 2C-F2; TRACKED: SA-2/SA-3, #18 (2B-F3/#17 FIXED `@0ce638b`). |
| RW-3 | Exception entry + syscall surface | **CLOSED `@01e1b40`** — 0 P0 / 0 P1 / **4 P2** all FIXED (R1-F1 userland_enter I-24 die-check gap; R2-F1 explicit_bzero secret-retention; R3-F1 OEXEC execute→read leak [masking-bug stack]; R4-F1 err.rs -1→EPERM alias) + R2-F2 thread_exit backstop + ERRORS.md coherence (multi-thread carve-out stale post-RW-1-C-F1, both reviewers). 4 Fable reviewers + Opus self-audit + round-2 on the chokepoint fixes (CLEAN). The chokepoint is SOUND (#713 trampolines, die-check coverage, uaccess+alignment, I-2 strip, SYS_NOTED restore, I-28, two-axis gate). 811/811; SMP gate 0 corruption. Catalog: `docs/holotype/{00-register,03-exception-syscall}.md` + `audit_holotype_rw3_closed_list.md`. REGISTERED → RW-13: R1-F2 snare:fpe, R4-F7 -T_E_* rollout re-vote (user), R4-F3 offset asserts, doc cluster. |
| RW-4 | Namespace/FS + 9P + Loom(DELTA) | **CLOSED `@ee30f55`+`@6cf5933`** — 4 Fable reviewers (R1 ns/resolver, R2 FS dev drivers, R3 9P stack, R4 Loom-DELTA) + Opus self-audit + a **dirty-close round-2** (2 Fable on the fixes). **2 P1 + 3 P2 + 2 P3 FIXED** (SA-F1 the per-Territory `ns_lock` [the #848 race, promoted P3->P1 by the P6 multi-thread lift; R1 rated it dormant, OVERRULED]; R2-F1 byte-mode `/srv` single-waiter extinction; R3-F1 9p owned-reply fail-closed; R4-F1 Loom SQPOLL race; **R-B-F1 round-2 caught R3-F1 over-broad** [local fid-full latched the shared FS dead] -> fixed; R2-F2 lseek `seekable` flag; R1-F1 + R-A doc/hygiene). The P6-multi-thread-lift systemic theme (shared with RW-2) recorded as scripture (DEBUGGING-PLAYBOOK 6.15 + CLAUDE.md). 814/814 (+3 regressions); SMP gate 0 corruption (x2). Catalog: `docs/holotype/04-namespace-fs-9p-loom.md` + `audit_holotype_rw4_closed_list.md`. REGISTERED -> tasks #23 (H items: R3-F2/F3/F4, R4-F2/F3/F4) + #24 (ref-doc currency). |
| RW-5 | Handles/caps/identity/perm | **CLOSED** — 4 Fable reviewers (R1 handle table #844-DELTA, R2 caps/legate/identity, R3 the `cap` device, R4 perm.c + /proc kill; all `claude-fable-5` start==end) + Opus self-audit. **0 P0 / 0 P1 / 1 P2 / 8 P3, all fixed.** Headline = the recurrent multi-thread-lift theme: A-4a atomized one `p->caps` writer (`proc_become_legate`) and missed the other writer (`devcap.c:327` hostowner `\|=` → lost-update race; P2 fail-safe, triple-converged R2/R3/self) + the readers (perm.c/devproc.c/peer_snapshot, P3 C11/TSan). + perm hardening (want==0 fail-closed; wstat unknown-bit reject — 2 regressions), the I-25 member-unelevated `_Static_assert` pin, handle.c KOBJ_SRV/LOOM defense-in-depth, 3 stale comments. The I-2/I-22/I-25/I-26/I-27 spine SOUND. 816/816; SMP gate 0 corruption. Catalog: `docs/holotype/05-authority.md` + `audit_holotype_rw5_closed_list.md`. REGISTERED → #23-class: the `proc_caps_load/or` accessor sweep + the multi-thread TSan caps harness + R4-H4 /proc-mount horizon. |
| RW-6 | corvus/joey/login | **CLOSED** — 4 Fable reviewers (R1 crypto core, R2 protocol/session/auth, R3 identity/clearance DB, R4 joey/login; all `claude-fable-5` start==end, no fallback) + Opus self-audit (crypto + session, double-covered). **0 P0 / 0 P1 / 5 P2 / 14 P3.** A mature thrice-audited surface (a5c/828/a1b) under a fresh FULL pass — the secret-handling + auth spine SOUND. 4 P2 FIXED: F1 crvs_v1_unpack Argon2-cost envelope (tampered-header KDF-DoS, converged R1=self); **F2 ADMIN_ELEVATE/RECOVER(system) cached-peer→live `SYS_SRV_PEER` re-query (C-22; the "console immutable" comment was false)**; F3 handle_user_create UPG group-cap (the a1b-F1 sibling boot-brick); F4 login passphrase/token scrub-on-error-leg (#828 A-F1 extension). 1 P2 DEFERRED → #876 (AUTH rate-limit; v1.0-unreachable, fix needs the time-decay). + 9 P3 fixed (hygiene/asserts/getty-backoff/relinquish-fall-through) + 5 P3 doc/track. NOT a dirty close (P1+P2=5<6; localized fixes). 816/816 + ADMIN_ELEVATE/RECOVER(user+system)/CLEARANCE→legate/login all E2E-green + 0 EXTINCTION; SMP gate 0 corruption. Catalog: `docs/holotype/06-services.md` + `audit_holotype_rw6_closed_list.md`. REGISTERED → #28 (H items: F9/F17/F18/F19 + #876 strengthen + the corvus multi-conn test). |
| RW-7 | Drivers/HW + Halls | **CLOSED** — round-1 4 Fable (R1 gic/timer/irqfwd, R2 uart/cons, R3 virtio/mmio/dma, R4 chacha/random/halls; all `claude-fable-5` start==end, no fallback) + Opus self-audit; **dirty close**: round-2 (2 Fable, A irqfwd+virtio / B console) + round-3 (1 Fable, virtio fixes), each + self. **3 P1 + 3 P2 (round-1) all FIXED**: R1-F1 irq single-waiter extinction `@0c0e484`; R3-F1 DMA-into-freed-pages-on-driver-death `@3ec134e`; R2-F1 Ctrl-C-after-SAK kills the trusted login authority `@2608c88`; + R1-F2/R3-F2/R2-F2 (P2). **Round-2 found a 4th P1 + a P2 on the R3-F1 fix** (the quiesce walked only handles → missed the mmap-then-close-fd MMIO device on its VMA; + the in-range reset raced the kernel RNG slot, converged with self-audit SA-R2-1) — both FIXED (the `p->vmas` walk + the RNG skip); **round-3 CLEAN** (0 P0/P1/P2; 2 P3 trust-envelope residuals). The recurrent P6-multi-thread-lift theme a 4th time: all four P1s in the death-path / wait-wake / cons→notes→SAK seam, none in the crypto or GIC mechanics (R4 = 0 P0/P1/P2). + 8 P3 fixed + 1 tracked (#35) + H-items registered (HT07.*). 823/823 + login E2E green + 0 EXTINCTION; SMP gate 0 corruption. Catalog: `docs/holotype/07-drivers-hw.md` + `audit_holotype_rw7_closed_list.md`. REGISTERED → #34 (virtio-input harness flake), #35 (bringup Ctrl-C decide+document), HT07.* H-items (per-device-kobj, hangup note, VERSION_1, board-drift, chacha KATs). |
| RW-8 | libthyla-rs + pouch | **CLOSED** (`cc79551` + Stratum `bf0cde0` + round-2 `9db9300`) — 5 Fable reviewers (R1 core / R2 fs-wait-cap-9p / R3 hardware-loom-DELTA / R4 virtio-drivers-bdev / R5 pouch-patches; all `claude-fable-5`) + Opus self-audit + a **dirty-close round-2** on the fixes. **1 P1 + 6 P2 (round-1) + 1 P2 + 1 P3 (round-2) all FIXED.** The two highest-stakes surfaces (the syscall-number table native+pouch; the SO_PEERCRED A-3 channel) verified CLEAN. **P1**: R1-F1/R2-F1 BufReader uninit-heap exposure on the read-error leg (triple-converged; UB + info-leak; regression `bufreader_error_leg`). **P2**: R5-F1 kernel NDFLT now cascades a multi-thread pouch daemon's uncaught SIGTERM (was swallowed — the death/notes seam); R5-F2 the seam check regained 13 dropped retargets; R5-F3 the pouch poll() compacts POSIX-ignored negative fds (was busy-spinning); R4-F2 virtio-input `u16`-truncation OOB; R4-F1 Stratum bdev `avail_idx` desync (hang/silent-stale-read); R4-F3 bdev fsync durability (cache=writethrough). **Round-2 earned its round**: RND2-F1 the poll compaction's `nk==0` boundary hit the kernel `nfds==0` reject (EPERM) → fixed (0/ENOSYS, matching 0005) + RND2-F2 stale 0007 NDFLT comment; the three load-bearing fixes (NDFLT cascade, bdev latch, BufReader reset) prosecuted SOUND → no round-3. The recurrent P6-multi-thread-lift / substrate-moved theme a **5th time** (the pouch boundary: each layer was correct when it landed, then #809/#811/A-3/stalk moved under it). 823/823 + login E2E (FS via bdev) + 0 EXTINCTION; SMP gate PASS — 0 corruption (default+UBSan × smp4/smp8). Catalog: `docs/holotype/{00-register,08-userspace-runtime}.md` + `audit_holotype_rw8_closed_list.md`. REGISTERED → #46 (the pouch POSIX-coherence + drift-defense revisit: R5-F4/F5/F6/F8/F9, R3-F3, R4-F7, RND2-F3, the 0007/0011/0012 patch-header sweep). |
| RW-9 | Utopia stack | **CLOSED (dirty close, CONVERGED CLEAN over 3 rounds)** -- 4 Fable reviewers (R1 lexer+parser, R2 eval+jobs+notes+glob+env+builtins, R3 line-editor+REPL+main, R4 coreutils; all `claude-fable-5` start==end) + Opus self-audit; then a round-2 + round-3 dirty-close re-audit (Fable, start==end). **1 P1 + 11 P2 + ~22 P3 (round-1) -- the MOST of any RW round** (the recurrent substrate-moved theme a 6th time: LS-4/LS-5/#811/#926 moved under the eval/notes/coreutils boundaries). The notes/death/job seam CORE is structurally SOUND. Round-1: P1 + 10 P2 FIXED (`07a27c9` command-sub hang + converged recursion caps, `c1aea9d`+`dcc4d08` coreutils, `cd7eae2` notes/Ctrl-C [+ removed the ls-5.exp `\r` VERIFY-AROUND]); R3-F2 multi-line render REGISTERED (cosmetic + invasive + no screen-state harness). **Round-2 caught the round-1 recursion fix was INCOMPLETE** (bounded brackets, NOT operator chains `!!!`/`**` -> still overflowed -> shell death; RND2-F1 P2) + a tr P3 + a self-audit P3 -- all FIXED `@aae24db` with non-vacuous regressions u-subst-test #13/#14. **Round-3 re-prosecuted the F1 fix by exhaustive call-graph enumeration: CLEAN 0/0/0/0** (firm verdict: parse_unary + parse_pow ARE the only unbounded operator vectors). 823->824 tests; SMP gate PASS 0 corruption; ls-5 interactive E2E all 4 cases PASS; 0 EXTINCTION. LESSON: a "complete" recursion fix can miss a vector class the regression test can't see -- the mandatory dirty-close round-2 is what caught it. Report: `docs/holotype/09-utopia-stack.md`; closed list `audit_holotype_rw9_closed_list.md`; register HT09.*. Registered follow-ups -> #54. |
| RW-10 | Cross-cut: consistency + invariant ledger | **CLOSED** — 4 Fable reviewers (A ledger I-1..I-12 / B I-13..I-21+I-31 / C I-22..I-30 + SPEC-TO-CODE drift / D rubric; all `claude-fable-5`, no fallback) + Fable main-loop self-audit. **0 soundness findings — 31/31 §28 enforcement mechanisms present, located, tested (0 ABSENT)**; 15 X-lens findings (5 H2 + 4 H3 + 6 H4 after merge): 12 FIXED in-arc (the §28 cell pass [12 cells incl. I-17 design-target + I-20 unbuilt marked honest]; the CLAUDE.md mirror re-sync 21→31 + phantom `mm/vm.c`/`mm/wxe.c` paths in both audit tables; `make specs` made honest [could not fail before] + the nine-specs fossils → the 17-module inventory; SPEC-TO-CODE: 4 missing sections written [asid/sched_alpha/sched_oncpu/pipe] + 5 currency notes + honest header; the I-27/SAK scripture the RW-7 sweep missed; STALK-DESIGN #957 annotation; the 10-item doc-per-chunk cluster from RW fix commits; + 2 new regressions `notes.snare_forge_rejected` + `9p_client.rlerror_hostile_ecode_bounded`; thread_spawn `-T_E_*` names; 6 ELF ABI pins) and 3 REGISTERED (#56 tiered spec-gate runner; #57 namespace-layout seams /dev+/proc+/ctl + §9.4 reconcile [USER scope call at RW-13]; #58 spawn-bypasses-namespace exec seam). 2 candidates withdrawn after verification (TTrace hygiene — already ignored; the LOOM.md §7 cite — correct). NOT a dirty close (0 P0/P1/P2; doc/test/tooling fixes only). 825/825 (+2) + boot OK + 0 EXTINCTION. Report: `docs/holotype/10-consistency.md` (the invariant ledger is §1); register HT10.*; closed list `audit_holotype_rw10_closed_list.md`. |
| RW-11 | Cross-cut: performance | **CLOSED** — 4 Fable reviewers (R1 memory / R2 sched-locks-tick / R3 IPC-9P-FS / R4 boot-entry-uaccess; all `claude-fable-5`, no fallback) + the main-loop empirical leg (3 timestamped HVF boots + 1 TCG; the budget's own claimed substrate) + self-audit. **0 soundness findings across ~31 perf findings — the audited mechanisms (elected reader, #844 snapshot, SQPOLL park, ASID 1-CAS fast path, die-check, #713 windows) are cheap or SOTA-matching.** Budget × measured (§4.5): syscall floor **83 ns measured** (<200 ns MET ×3 boots); 9P loopback full-path **24–61 µs** (floor inside the 50 µs p50); first banner 89 ms (non-test fraction fits 5×); IRQ-path p99 reasoned-fits (0.6–1.4 µs). THE HEADLINE (HT11.SA-1 ≡ R2-F1, independently converged + empirically pinned): **the no-wake-preemption slice cliff** — irq-bench = 6.0224–6.0236 ms σ≈10 µs = EXACTLY `THREAD_DEFAULT_SLICE_TICKS(6)`×tick; same-CPU wakes never preempt, so under saturation every rendez wake waits up to 6 ms (~300× the p99.9 cells) → **V1.0-RISK** for the Phase-7 budget gate → #60 (RW-13, folds into the 2A-F6 EEVDF reconcile; the fix is additive). Twin V1.0-RISK (R4-F1/F2): **no production boot configuration exists** (unconditional 825-test suite + joey's argon2-bearing probe ladder; the kernel's own `boot-time:` line self-reports 1249–1265 ms vs 500 ms; non-test ≈ 0.1 s) → #61. Bench honesty (SA-1a): irq-bench has only ever measured the test-mode slice-wait path (pre-notify-enable, secondaries quiescent) — 41-irq-bench.md + VISION §4.5 gating notes corrected in-arc. The ranked V1.X backlog (→ #62, 16 register rows): per-byte uaccess (a function call PER BYTE on every IO copy — top leverage) > stalk's ~2N+2 RPCs/open > msize 4096 + 2-frame ring (257 RTTs/MiB) > per-RPC reply kmalloc > the eager-memory cluster (per-page TLBI loops, 16–43 ms attach-zero under vma_lock, pow2 eager-commit) > handle-table toll (#844 re-audit) > the coarse-global-lock set (torpor/proc-table/TTAS) > SLUB-without-its-fast-path (ARCH §6.4 honesty-marked) > eager-FP/tick/O(N)-rq/broadcast-IPI > spawn double-copy. In-arc edits: 6 trivially-mechanical honesty fixes ONLY (3 stale kernel comments incl. the magazines header contradicting its code + ARCH §6.4 DEFERRED markers + VISION §4.5 measurement-status truth + the 41-irq-bench correction) — NO perf rework landed, per charter. NOT a dirty close (0 soundness; doc/comment edits only). 825/825 + boot OK + 0 EXTINCTION post-edit. Report: `docs/holotype/11-performance.md` (the budget table §1 + the lock-granularity inventory §3); register HT11.*; closed list `audit_holotype_rw11_closed_list.md`. |
| RW-12 | Cross-cut: gaps | **CLOSED** — 4 Fable reviewers (R1 W1-fileservice + W2-buildstorm / R2 W3-interactive + W6-editor / R3 W4-network / R4 W5-containers; all `claude-fable-5` start==end, no fallback) + the main-loop self-audit (SA-1..SA-7, merged at equal authority) + the aux `DOC-GAP-REPORT.md` (G01-G18). **60 gap findings, 0 H1 / 19 H2 / 29 H3 / 12 H4 — inventory + registration only, ZERO in-arc edits** (unlike RW-11's 6 mechanical fixes, every honesty fix here is coupled to an RW-13 scope/emerge decision -> the user's vote). Two dominant themes: **(1) present-tense scripture over deferred surface + pervasive phase-record incoherence** — VISION/NOVEL/COMPARISON/ARCH present-tense capabilities the tree never built (per-Proc 9P connections, union mounts, advisory locks/xattr, `/ctl/9p` observability, the nine-specs claim), and the phase tables disagree after the LS arc + convergence detours + Pouch renumbering re-scoped work without reconciling the records: the **network phase is named 5 ways** (Phase 6/7/8; VISION:315->ARCH §13 now-dangling), **ARCH §23.8 lists Helix+PTY+/tmp-tmpfs as Phase-7-exit must-haves** that LIFE-SUPPORT re-scoped to Phase 8/LS, and **I-20 says PTY "lands with LS-8" when LS-8's scope excludes the master/slave mechanism** — the RW-13 reconciliation input. **(2) two keystone capabilities unbuilt with no/weak re-entry**: exec-from-namespace (all 5 spawn variants `devramfs_lookup` from the boot cpio -> a container's binaries are categorically unexecutable -> the keystone blocker of the container ✓; #58 raised H3->H2), and the Resource/DoS floor (IDENTITY-DESIGN §769 commits it under BUILD; unbuilt -> memory/thread/proc bombs unbounded; the v1.x quota seam depends on its counters). Third thread: **namespace introspection has no substrate** — `/proc/<pid>/ns` is a `binds:N` stub and `struct Spoor` retains NO name field, so the Plan 9 `ns` tool can't be rendered without a mount-name-retention data-model change. The kernel/runtime substrate is broad + sound (RW-1..11); the gaps are the *userland* surface a workload needs (a toolchain, an editor, termios/PTY, a clock, env, sockets) — almost all on the recorded LS-arc / Phase-8 / Phase-9 horizon — + the reconciliation debt between scripture and tree. NOT a dirty close (0 soundness; nothing built/edited). Report: `docs/holotype/12-gaps.md` (the workload×gap matrix §2 + the keystones §3 + the phase-incoherence cluster §4 + the RW-13 input §5); register HT12.*; closed list `audit_holotype_rw12_closed_list.md`. Registered -> #64 (the scripture-reconciliation pass), #65 (Resource/DoS floor), #66 (ns introspection substrate), #67 (userland completeness/toolchain), #68 (W4 net design-pass charter), #69 (LS-arc sub-seams), #70 (container-runner re-anchor); #58 raised to H2. |
| RW-13 | Consolidation + emerge (scripture commit) | **CLOSED** — the arc capstone. RW-0..RW-12 all closed; the specimen is SOUND (0 P0 across the whole system; ~15 P1 + ~40 P2 all fixed in-arc; the 3 cross-cuts returned 0 soundness). The single systemic finding: the P6 multi-thread-Proc lift outran the wait/wake machinery, and almost every P1 lived there (RW-2/4/5/7/8/9). The four v1.0 scope forks VOTED 2026-06-11: networking IN (Phase 8), containers BUILD-the-core (#58 exec-from-ns + #66 ns substrate; union-mounts -> v1.x), on-system toolchain IN (Phase 8, NEW scope), pre-rc hardening FIRST (#60/#61/#65). Re-planned roadmap + register triage + emerge sequence: `docs/holotype/13-consolidation.md`. The arc EMERGES here -> the system resumes building. Next chunks: #64 reconciliation -> the D4 pre-rc sub-arc -> the container keystone -> the LS line (Phase-7 completion) -> Phase 8 -> Phase 9 rc. |