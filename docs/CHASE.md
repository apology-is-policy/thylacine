# CHASE — the host-parity run-down (EMERGENCY ARC; ACTIVE)

**Status: ACTIVE — this arc PREEMPTS all other work.** Declared by the user
2026-07-11: *"let's preempt, and enter an emergency arc with a singular
focus. I appoint you a dictator with the sole purpose to chase down the
perf diff and either achieve host parity on gofmt with a tolerance of
200 ms, or prove that it is impossible to achieve due to past foundational
OS design decisions."*

The name: a chase is the apex predator's run-down — not an ambush, a
sustained pursuit to the end. This document is the dictate: the single
source of truth for what the project is doing until the arc exits. Every
session that picks up this tree works THIS arc and nothing else (the
standing queue is suspended; see §7).

---

## 1. The mandate

Exactly one of two exits:

- **PARITY**: the on-device `go build` of `cmd/gofmt` lands within
  **+200 ms of the host** on every bar-bound workload/state pair (§3),
  reproducibly (N ≥ 3 pool-restored boots, quiet-host-verified), on the
  shipped configuration.
- **PROOF OF IMPOSSIBILITY**: an audited floor ledger (§6) demonstrating
  that named FOUNDATIONAL design decisions impose a floor above the bar —
  each entry carrying (i) the decision, (ii) the mechanism by which it
  costs time, (iii) its **measured** floor contribution, (iv) what change
  would remove it and why that change is foundational (an ARCH-level
  redesign, not an optimization). The proof is prosecuted by a Fable-5
  adversarial round like any audit-bearing close: a hole in the ledger
  (an unattributed millisecond band, an unmeasured hypothesis, a "floor"
  that is actually fixable) fails the proof.

A partial outcome is expected and honest: some (workload, state) pairs may
reach parity while others terminate in the ledger. The arc exits when
EVERY bar-bound pair has one of the two dispositions.

## 2. The dictatorship — what it changes and what it does not

- **Preemption**: tasks #13 (GCP sanitizer batch), #2 (bug register),
  #28, #30, and the perf-mission-close question are SUSPENDED for the
  arc's duration. No session picks them up. (Exception: a SOUNDNESS
  defect surfaced by chase work follows CLAUDE.md stewardship as always —
  correctness still outranks the chase.)
- **Authority**: full autonomy over both trees (Thylacine + Stratum) for
  measurement, instrumentation, and fix arcs, per the standing grant.
- **What the dictator still cannot do** (the senate's reservations, per
  CLAUDE.md): push (the user pushes), break on-disk/wire/ABI formats
  without signoff, spend external money (GCP stays propose-then-execute),
  or silently deviate from ARCH — a fix that needs an ARCH change gets a
  scripture commit + user signoff first, exactly as in peacetime.
- **Rigor is not suspended**: fix sub-arcs inside the chase keep the
  full discipline — scripture-first, spec where invariant-bearing,
  focused audits on audit-bearing surfaces, SMP gates on kernel/server
  changes. Speed comes from focus, not from cutting the bar.
- The STMD26/DIAG23 instrument stacks are the arc's eyes and STAY in the
  working trees (their strip-before-commit dance per
  `audit_rc6_closed_list.md`).

## 3. The bar — precise and matched

**Reference machine**: the same M2 host. **Toolchain**: the same
go-thylacine fork — darwin-native binaries on the host, the
thylacine/arm64 cross on the device. **Both sides measured in the same
session**, host quiet (§5). **Device configuration**: the shipped boot
(workers-ON adaptive dispatch) at the device's **best measured smp —
currently smp=4**. The original smp=8 rule was measured WRONG at C-0
(clean sentinels, same boot lineage: S1 974 vs 675 ms, S3 10 605 vs
5 267 ms — smp=8 loses 1.4–2.0×): 8 vCPUs on the 4P+4E M2 under HVF
straggle on E-cores the guest cannot see. Parity of the machine means
BOTH sides at their best honest configuration on the same hardware — the
host keeps all 8 cores natively; handicapping the device with a config
that hurts it serves nothing. The inversion itself is a ledger band
(H5 sharpened: why does the device NOT scale to 8 vCPUs when the host
scales to 8 cores — vCPU E-core placement / IPI+wake costs / -p 8 FS
queueing are the candidate mechanisms).

**W1 (the bar workload)**: `go build -o <out> cmd/gofmt` (91 packages,
real program, source baked).

| state | definition (identical both sides) | bar |
|---|---|---|
| **S1 all-warm** | second consecutive build in the same boot/session; GOCACHE warm; all caches hot | device ≤ host + 200 ms |
| **S3 fully-cold** | a FRESH empty GOCACHE on the real FS immediately before the build (both sides) — the `go clean -cache` state realized without paying the deletion storm inside or adjacent to the measured window. Device: `/gocold` on the pool (same substrate as `/go-cache`); host: `mktemp -d` on APFS. Same boot/session, so OS/guest file caches are warm-ish but the build cache is empty | device ≤ host + 200 ms |
| S2 boot-cold, cache-warm | first build after boot (device) | **diagnostic only** — the host analog needs a page-cache purge (sudo); measured once if the user assists, never bar-bound |

**C-0 as-built** (2026-07-11): device S1 = the joey go4c `gofmt-warm` line;
device S3 = the new `gofmt-s3cold` line (fresh `/gocold` + a staleness guard
that VOIDs the number on a non-restored pool). W2 = `w2compile-cold/-warm`
lines, gated on a `/chase-w2` pool marker baked only under
`THYLACINE_CHASE_W2=1` (SMP gates and normal boots never pay a compiler
build); its cold runs on its own fresh `/gocold2` (the S3 gofmt build
part-warms `/gocold` with shared stdlib deps). smp=8 =
`THYLACINE_TEST_CPUS=8` (no code change; test.sh had the knob). The
sanctioned driver is `tools/chase-bench.sh` (sentinel-wired per §5:
`device [N]` / `host [N]` / `host-w2 [N]`).

2026-07-11 opening positions (quiet host, N=2): S1 device 669/711 ms vs
host 40–50 ms (**gap ~630 ms**); S3 host 2.43 s, device unmeasured-matched
(the ~21 s twin bench is seed-cold at smp=4 — C-0 produces the honest S3
number via in-guest `go clean -cache` at smp=8).

**W2 (the reference workload — triangulation, NOT bar-bound)**:
`go build cmd/compile` — the Go compiler itself, the heaviest pure-std
real program. Chosen to maximally separate the hypotheses: it is
compute-dominated at cold (if the device tracks the host there, HVF CPU
is exonerated and the gap is invocation/FS machinery; if it does not,
the floor is deeper) and invocation-dominated at warm no-op (sharpening
the per-invocation floor measurement). Requires a bake extension for its
source closure (C-0). Swapping W2 for another large project needs no
re-chartering — it is a lens, not a goal.

**The tolerance is absolute**: +200 ms per pair, not a ratio. At S1 that
means the device warm build must finish in ≤ ~250 ms — a −63% from
today; at S3, ≤ ~2.6 s.

## 4. The hypotheses (the floors to be hunted, not assumed)

The candidate foundational decisions the impossibility branch would
indict — each enters the arc as a HYPOTHESIS with a measurement plan,
never as an excuse:

- **H1 — the userspace FS server**: every FS op crosses
  guest-kernel → 9P → stratumd and back. Mitigated by the Larder /
  cached-open / POUNCE / bulk rings / RC concurrency; the residual per-op
  and per-build costs must be measured, not extrapolated.
- **H2 — encryption + integrity at rest**: AEAD + Merkle per extent
  (hardware AEGIS since CF-4 A). Residual cost on the build's actual read
  set; interacts with the dcache (server) + Larder pages (guest).
- **H3 — the per-invocation tool floor**: a build is ~dozens of process
  spawns (compile/asm/link invocations + `go` itself). Each pays
  spawn + ELF load + page-in of an ~18 MB tool + runtime init. REVENANT's
  qid-keyed Image cache should make repeats near-free — WHY it does not
  (or does and something else dominates) is the first question of C-1.
  Sub-floors: SYS_SPAWN vs posix_spawn, demand-page fault storms vs
  macOS's dyld shared cache, Go runtime init (the vDSO killed the
  clock_gettime storm; what remains).
- **H4 — scheduler/vCPU wake latency under HVF**: futex/torpor wake
  chains, the tickless deep-park resume cost (#299/TI-4e), cross-vCPU
  IPIs. Affects the many-goroutine build graph.
- **H5 — parallelism ceiling**: smp=4 vs the host's 8 cores (retired for
  bar runs by the smp=8 rule); the FS-action serialization the RC arc
  attacked — measure what `-p 8` now buys on-device.
- **H6 — the 9P transport itself**: msize framing, ring hops,
  syscall-per-op at the guest boundary vs macOS's in-kernel VFS + unified
  page cache. The deepest structural candidate — if after H1–H5 are paid
  down the residual sits in irreducible per-op boundary crossings times
  the build's op count, THIS is the ledger's likely spine.

## 5. The measurement law (the burner lesson, codified)

Every bar-relevant number obeys:

1. **Quiet-host sentinel**: before and after each run, load average and a
   `ps` sweep for stray >50%-CPU processes; both recorded in the run log.
   A dirty sentinel voids the run. (2026-07-11: four orphaned CPU burners
   from a test loop silently inflated every number ~1.8x for an hour —
   three CONSISTENT boots still lied. Consistency does not exonerate the
   host; a persistent confound is consistent.)
2. **Matched cache states** per §3 — a number without a named state is
   not a number.
3. **Pool snapshot-restore per device boot**; same-session host
   measurement; N ≥ 2 for direction, N ≥ 3 for a bar claim.
4. **Burner/stress helpers capture `$!` PIDs and verify their kill with
   `ps`** — never `jobs -p` in a script.
5. Instrument deltas (DIAG23 op counts, STMD26 service ticks) ride every
   run — a wall-clock move without a mechanism attribution is not
   accepted as progress.
6. Known environmental flares (C-0 observed): **macOS Spotlight**
   (`spotlightknowledged`/`mds`) re-indexes the 2.5 GB pool.img after
   every per-boot restore and can burn a core mid-run — the sentinel
   catches it (a one-time fix is adding `build/` to Spotlight Privacy;
   needs the user's GUI session). The sentinel's stray check is
   2-sample persistence (5 s apart, pid-matched): the burner class it
   defends against is persistent; single-sample spikes (a finishing
   pipeline, the agent's own turn processing) decay before the measured
   window and must not void runs.

## 6. The floor ledger (the arc's central artifact)

`docs/chase/LEDGER.md` (created at C-1): the gap decomposed into named,
measured, non-overlapping bands that SUM to the observed delta per
(workload, state). Each band carries: mechanism, evidence (boot +
instrument lines), disposition — one of:

- **FIXED** (landed in-arc; band re-measured to ~0),
- **FIXABLE-VOTED** (a fix exists but is big/ABI-bearing/ARCH-touching —
  surfaced to the user with cost),
- **FOUNDATIONAL** (the impossibility entry: decision + mechanism +
  measured magnitude + the redesign that would remove it).

An unattributed band means C-1 is not done. The ledger's FOUNDATIONAL
entries, if the bar is not met, become the impossibility report —
prosecuted adversarially (§1) before the arc may exit on that branch.

## 7. The plan

- **C-0 — arm the range**: the honest S3 protocol (in-guest
  `go clean -cache`); smp=8 bar configuration; the W2 (cmd/compile) bake
  closure; the quiet-host sentinel wired into the bench scripts; fresh
  full-instrument images (the current baked stratumd carries defs-only
  D26 — rebuild).
- **C-1 — decompose S1**: the ~630 ms warm gap attributed to bands
  (per-invocation floor × invocation count first — H3; then per-op FS
  residuals — H1/H6; then sched — H4). The ledger is born here.
- **C-2 — decompose S3** (after C-0's honest number): compile-CPU vs
  FS/AEAD vs invocation bands; the W2 cold triangulation (H-separation).
- **C-3..N — fix sub-arcs**, biggest band first, each scripture-first
  with its own verification; re-measure the band + the bar after each.
- **C-final — the exit**: PARITY (bar table green, N ≥ 3) or the
  IMPOSSIBILITY REPORT (ledger complete, adversarially audited). Either
  way: the arc close updates this doc's status line, the register, and
  memory; the suspended queue resumes.

## 8. Suspended until arc exit

Tasks #13 / #2 / #28 / #30; the perf-mission-close ask (subsumed — the
chase IS the mission's end-game); the nora-under-HVF bug (unless it
blocks chase tooling). The RC/CF/Larder/POUNCE closed lists and the
mission register (`project_go_build_clean_perf.md` in session memory)
are the arc's data trove — read them before re-deriving anything.
