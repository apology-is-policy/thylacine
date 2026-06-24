# Stratum Stabilization Arc — Charter

Binding charter for the proactive hardening of Stratum against the Thylacine
workload. Stratum is in scope (`~/projects/stratum/v2`, branch
`thylacine-pouch-arm`); Thylacine is its only client, so the bar is "whatever
serves Thylacine." This document is scripture for the arc: areas, method, exit
criteria. Redesigns surface as their own scripture commits first (per
`CLAUDE.md` "Design conversation -> scripture commit").

---

## Thesis

The on-device `go build` is the first workload to stress Stratum's write path
with **large multi-record files, non-aligned interior rewrites, heavy
concurrent writes, sustained I/O, and a storm of small files**. Stratum is
feature-complete with extensive tests + TLA specs — but those exercise **small
files, aligned writes, light concurrency**. An entire workload class went
untested, and its latent defects are surfacing serially: amplification (#352),
extent-overwrite data-loss (F1), drift corruption (#355), bdev one-error-death
(#2), errno masking (#3), srvconn back-pressure (#348/#349) — and, tellingly,
**two reproducible data-loss bugs in the very fix written to close the first
one** (the deep-fix F1/F2, found by the convergent audit).

That last point is the thesis: **reactive point-fixes into this surface are a
liability.** A deliberate, reviewed, data-corruption-class fix still shipped two
corruption paths. The arc replaces whack-a-mole with **proactive, convergent,
per-area hardening**, grounded in an **executable workload harness**, measuring
**both correctness and throughput**. The `go build` then becomes the final E2E
victory lap — not the bug detector.

---

## Two co-equal goals

### G1 — Correctness: zero silent data loss

Every interaction-type area converges **adversarial-audit-clean**: a focused
holotype-reviewer round (Opus, max effort, per `feedback_reviewer_model`)
returns **0 P0 / 0 P1** across consecutive rounds, with a **committed regression
test per finding** that fails on the pre-fix code (non-vacuous). Findings that
are fundamental (not point-fixable — F1 was) **escalate to redesign**, surfaced
as scripture, rather than patched around.

### G2 — Throughput: COW-FS parity modulo crypto

Stratum's **only** accepted bandwidth/latency delta versus a mature COW FS
(BTRFS baseline) is the **cryptographic layer** — and within that, **AEAD
encryption is the dominant Stratum-specific cost** (integrity/Merkle hashing is
near-parity, since BTRFS/ZFS checksum too). Everything else — the COW
machinery, the extent/btree layout, the write/read path, metadata ops, the
flush — must be **COW-FS-competitive**, on **both** axes:

- **Large files** — sequential write/read bandwidth (MB/s) within the crypto
  delta of BTRFS.
- **Pleiades of small files** — create/write/read ops/s competitive; low
  per-file + per-metadata-op overhead (the go-build object-file storm).

The accepted crypto delta is **measured, not assumed**: every throughput bench
runs with and without the AEAD layer so the encryption cost is isolated.
Measured gaps beyond the crypto delta become **tracked perf debt**, never
silently accepted. (#352's 2.7x amplification was a throughput bug — 2.7x the
I/O — as much as a space bug; perf is a first-class exit criterion, not a
footnote.)

---

## Method — the convergent per-area loop

For each area, iterate until convergence:

1. **Harness** (host-side, deterministic, fast — no QEMU):
   - *Correctness repros* — the bug-finding workload for the area (large files,
     interior rewrites, drift, concurrency, error/exhaustion injection), using
     the **differential-reproduction** pattern (build fix + pre-fix binaries,
     plant + corrupt, **measure byte-level loss**) so findings are proven, not
     argued.
   - *Throughput bench* — the area's workload vs a **BTRFS baseline** (loopback
     or tmpfs-backed), with/without AEAD to isolate the crypto delta.
2. **Adversarial audit** — the holotype-reviewer prosecutes **both**
   correctness (differential repro) **and** throughput hazards (amplification,
   RMW cost, per-op overhead, lock contention, allocation churn).
3. **Fix or redesign** — point-fix where sound; **escalate to redesign**
   (scripture-first) where fundamental.
4. **Re-converge** — repeat 2–3 until a round returns clean.

**Exit criterion per area:**
- 0 P0 / 0 P1 on a clean convergent round; every P0/P1 closed with a
  non-vacuous regression test.
- The harness (now permanent correctness + perf coverage) is green.
- The throughput bar is met, or the residual gap is quantified + tracked as
  named perf debt.
- An **as-built technical reference** for the area's subsystem is written or
  refreshed in the Stratum reference docs (`~/projects/stratum/v2/docs/`). The
  audit's line-by-line read is the source — Stratum's write-path internals are
  thinly documented today, and the arc is the moment they're understood deeply
  enough to document right (the Thylacine `docs/reference/NN-*.md` per-subsystem
  template applies: purpose / public API / implementation+invariants with
  file:line / data structures / state machines / spec cross-ref / tests / error
  paths / perf characteristics / known caveats).

Per-area closed-lists live in `memory/audit_stratum_<area>_closed_list.md`. The
dirty-close re-audit rule (`CLAUDE.md`) applies: an invasive fix gets a
follow-up round on the fix itself.

---

## The areas (by OS-interaction type; foundation/stakes ordered)

| # | Area | Stresses | Known defects |
|---|---|---|---|
| **A** | Extent write / overwrite / flush / coalesce — large multi-record write, grow, **interior rewrite**, drift | `extent_index`, `dirty_buffer`, `sync` write, `fs` write path | #352, **F1**, #355, deep-fix **F1/F2** |
| **B** | Read path + read-after-write + short-read coherence | `sync` read (the **single-extent** `read_extent` MVP — the F2 root), overlay, decrypt | the single-extent read (must become multi-extent) |
| **F** | Exhaustion + I/O-error handling **and recovery** | `bdev_thylacine` (the **failed-latch** = one fault kills the FS, #2), errno propagation (#3, both Stratum + the dev9p boundary) | **#2, #3** |
| **D** | Heavy concurrent writes through one session | `dirty_buffer`, the elected reader, locks, EBR | #348/#349 (fixed; surface load-bearing) |
| **S** | Pleiades — many-small-files throughput | inode/dirent create, metadata ops, small-write batching | — (perf-led) |
| **G** | Durability under load (commit / sync / fsync) | `sync` commit, `bdev` | — |
| **E** | Crypto/integrity throughput — the accepted-delta baseline | AEAD, Merkle, the dcache (#343) | #343 (uncommitted) |

**Ordering rationale.** **A** first — highest stakes (silent corruption), where
the active defects + the in-flight fix are. **B** is coupled to A (F2's fix
needs a multi-extent `read_extent`) so it lands with/just after A. **F** is the
foundation that turns a transient fault into permanent FS death — pull early.
**D** then **G** then **S**. **E** is established early as the *measurement
reference* (the crypto cost every other area's throughput is judged against),
even if its own optimization (the dcache) lands later. Throughput is measured
in **every** area; S + E are merely the throughput-led ones.

---

## Area A — round 1 (the concrete start)

The uncommitted deep fix (`src/fs/fs.c` `fs_write_extent_slot_locked` + the
recordsize split) is area A's round-1 starting point — ~80% sound (lookup
completeness, memcpy bounds, termination, lock order, the sequential fast-path,
and non-vacuity all audit-verified), but it ships **two reproducible
silent-data-loss bugs** (task #1):

- **F1** — the per-slot **clamp** truncates a **legacy cross-slot extent's**
  far-slot tail on the first overlapping sub-write after **upgrade** (hits every
  pre-fix pool, incl. the go-build device pool).
- **F2** — the RMW uses the **single-extent** `stm_sync_read_extent` (stops at
  the first extent), ignores the short count, and writes the full scratch, so a
  write spanning **two extents in one slot** zero-clobbers the second (fresh
  pool, public API).

**Round-1 corrected design (closes both):** compute the **unclamped**
overlap-expansion `[exp_off, exp_end)` (may cross slots for legacy extents);
**loop** the RMW read across the full span accumulating each extent's decrypted
bytes (the `read_full` shape — requires B's multi-extent read, or a local loop)
so scratch holds **all** live bytes before the overlay; overlay the new bytes;
then **split the covering write** at recordsize boundaries (each per-slot piece
replaces its part). Add regressions: **fresh-pool multi-extent-in-a-slot** +
**OLD-planted-cross-slot upgrade**. Re-audit (round 2) to convergence.
Throughput check: the sequential fast-path stays RMW-free; interior-rewrite cost
stays bounded at one slot.

---

## Discipline

- **Redesign-escape + scripture-first** — fundamental findings become scripture
  commits before code (the F1 lesson: don't patch around the disease).
- **Per-PR docs** — Stratum reference docs + this charter updated as areas
  close.
- **The go build is the E2E victory lap**, run after the areas converge — not
  the bug detector.
- **The harness outlives the arc** — the permanent correctness + throughput
  regression coverage Stratum was missing for the Thylacine workload.
- **The arc's outputs are three** — hardened code, the missing test/perf
  harness, and the **as-built technical reference** Stratum lacked (accrued
  per-area as each subsystem is understood deeply enough to document).
- **The arc is a bounded detour from the go-build (#342).** When the areas
  converge, **resume Go exactly where it paused** — re-bake the GOROOT pool
  with the stabilized Stratum, boot go-4c; the go build then progresses past
  #352/#355, correct *and* performant, to the next blocker or completion. The
  return-address recipe + the preserved Go-4c WIP are in
  `memory/project_next_session.md` (LATEST-8).

---

## Status

- Charter: **drafted** (this doc), pending user sign-off on scope.
- Area A round 1: deep fix uncommitted + unsafe (F1/F2); corrected design known
  (task #1). **Next.**
- Areas B–E: scoped, not started.
- Folded in: tasks #2 (bdev failed-latch) + #3 (errno) -> area F; #343 dcache
  (task #8) -> area E.
