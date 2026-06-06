# Loom — status

The **second of two pre-Utopia arcs** (Lazarus M1 first — now COMPLETE — then
Loom). Binding design: `docs/LOOM.md` (signed off 2026-06-05). Roadmap
registration: `ROADMAP.md §8.0a`.

## TL;DR

Loom is the io_uring inversion: userspace posts 9P-shaped ops into a shared-Burrow
submission ring; the kernel's #841 elected-reader 9P client drives them;
R-messages return as completion-queue entries. No new opcode namespace — 9P *is*
the uniform vocabulary, so the async batching layer covers files, `/net`, `/proc`,
`/srv`, and devices uniformly. Spec-first is **re-enabled** for this surface:
`specs/loom.tla` is TLC-green and gates every impl sub-chunk. **Loom-1 (the
model) is COMPLETE.**

## Landed sub-chunks

| Commit | Sub-chunk | What | Verification |
|---|---|---|---|
| `ebd1f7b` | **Loom-0** | scripture — `docs/LOOM.md` + the NOVEL.md promotion + the ROADMAP §8.0a/§12.2 registration. No code. | (docs only) |
| `7983794` | **Loom-1** | `specs/loom.tla` — the SQ/CQ op-lifecycle state machine + the cfg matrix. Pins **I-29** (completion integrity: no-lost / no-double / no-stale) + **I-30** (submit-time capability pin) + ring TOCTOU + CQ back-pressure. 12 safety invariants + 1 liveness property; clean cfg + a liveness cfg + 5 buggy-cfg counterexamples (live_sqe_reread → `ArgPinnedToSnapshot`, recheck_at_completion → `ObjPinnedToSnapshot`, double_post → `NoDoubleCompletion`, lost_on_full → `CqNeverOverfull`, stale_after_teardown → `NoStaleCompletion`). **TLC-green gates Loom-2..6.** | clean 582 distinct states; liveness (`EventuallyCompletes`) 678; each of the 5 buggy cfgs violates exactly its targeted invariant |

## Remaining work (Loom-2..6 — each spec-gated + audited)

- **Loom-2 — engine + ring**: the pluggable-completion refactor of the #841
  client (POST_CQE vs WAKE_RENDEZ; one engine, two front-ends; **audit-bearing**)
  + `SYS_LOOM_SETUP` + the Burrow-backed SQ/CQ memory layout + the
  registered-handle table. Audit.
- **Loom-3 — batch-enter core**: `SYS_LOOM_ENTER` (submit N / reap M) + SQE →
  `p9_client_<op>` dispatch + the submit-time pin (LOOM.md §8.5) + CQE post +
  out-of-order completion. The core. Audit.
- **Loom-4 — SQPOLL**: the kernel poll-thread (zero-syscall hot path;
  `cpu_pinned`-able) — wait/wake + lifetime surface. Audit.
- **Loom-5 — multishot + linked ops**: one SQE → many CQEs (the `/srv` accept
  loop) + LINK/DRAIN per-fid ordering. Audit.
- **Loom-6 — registered buffers + native API + bench**: pinned Burrow regions
  (zero-copy payload) + the libthyla-rs Loom wrapper + a latency benchmark on a
  high-fanout workload. The `LOOM_OP_WIRE_PASSTHROUGH` seam stays reserved
  (designed, not built). Final audit + arc close.

The ARCH §28 invariant-table edit reserving **I-29 + I-30** lands **with the
impl** (Loom-2), the way Lazarus deferred its §28 edit to W1 (docs/LOOM.md §9).
The CLAUDE.md / ARCH §25.4 audit-trigger row for the Loom surface
(pluggable-completion refactor, the SQ/CQ shared-memory async boundary, the ring
Burrow lifecycle, the submit-time pin, SQPOLL, multishot) lands with Loom-2 too.

## Build + verify

Spec gate (TLC; the clean cfg gates every impl sub-chunk):

```bash
export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"
cd specs
# clean (all 12 safety invariants) + liveness:
java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -config loom.cfg loom.tla
java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -config loom_liveness.cfg loom.tla
# buggy-cfg counterexamples (each must report its targeted invariant violated):
for b in live_sqe_reread recheck_at_completion double_post lost_on_full stale_after_teardown; do
  java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -config "loom_buggy_${b}.cfg" loom.tla
done
```

## Trip hazards (carry into Loom-2..6)

- **Spec-first is re-enabled for this surface.** TLC-green on `loom.cfg` is a
  pre-commit gate for every Loom impl sub-chunk; a new mechanism extends
  `loom.tla` FIRST, then the impl (docs/LOOM.md §7 + §10). Loom-2 adds the engine
  front-end + setup actions; Loom-4 the SQPOLL wait/wake; Loom-5 the multishot +
  LINK/DRAIN per-fid ordering — each adds its invariant surface to `loom.tla`.
- **I-30 is the load-bearing security property**: resolve + rights-check + pin at
  SUBMIT (the #844 by-value handle snapshot, object refcount held); NEVER
  re-resolve at completion. The `buggy_recheck_at_completion` counterexample is
  the io_uring credential-vs-work CVE class — an admitted op acting against a
  clunk+reused (wrong) object.
- **Ring TOCTOU**: copy every SQE field to kernel memory before validating, act
  on the copy — never re-read the shared ring after the check
  (`buggy_live_sqe_reread`).
- **CQ back-pressure**: never post into a full CQ; hold the completion until
  userspace drains a slot (the F3/F5 9P-client all-or-nothing-fail discipline;
  `buggy_lost_on_full`).
- **Teardown must quiesce in-flight ops** (the #811 death-interruptible unwind)
  before freeing the ring Burrow — else a late reply posts a stale CQE
  (`buggy_stale_after_teardown`).

## References

- `docs/LOOM.md` (the design) — §6 (soundness obligations), §7 (resolved design
  votes), §8 (the ABI sketch), §9 (I-29 / I-30 + the audit surface), §10 (the
  sub-chunk decomposition).
- `ARCHITECTURE.md §21` (the 9P client) + §28 (I-29 / I-30 reserved at impl);
  `ROADMAP.md §8.0a`.
- `specs/loom.tla` + `specs/SPEC-TO-CODE.md` (the loom section).
- `specs/9p_client.tla` (the elected-reader engine below this layer) +
  `specs/poll.tla` (the register-then-observe wait/wake lineage) — the spec
  cousins Loom builds on.
