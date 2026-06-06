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
model), Loom-2a (the ring substrate), Loom-2b (the pluggable-completion engine
seam), and Loom-3 (the batch-enter core — `SYS_LOOM_ENTER` + SQE dispatch + the
I-30 submit-time pin + the async-op container + the `loom_free` quiesce) are
COMPLETE.** v1.0 dispatches the no-payload opcodes (NOP / FSYNC); the payload
opcodes land with Loom-6's registered-buffer surface.

## Landed sub-chunks

| Commit | Sub-chunk | What | Verification |
|---|---|---|---|
| `ebd1f7b` | **Loom-0** | scripture — `docs/LOOM.md` + the NOVEL.md promotion + the ROADMAP §8.0a/§12.2 registration. No code. | (docs only) |
| `7983794` | **Loom-1** | `specs/loom.tla` — the SQ/CQ op-lifecycle state machine + the cfg matrix. Pins **I-29** (completion integrity: no-lost / no-double / no-stale) + **I-30** (submit-time capability pin) + ring TOCTOU + CQ back-pressure. 12 safety invariants + 1 liveness property; clean cfg + a liveness cfg + 5 buggy-cfg counterexamples (live_sqe_reread → `ArgPinnedToSnapshot`, recheck_at_completion → `ObjPinnedToSnapshot`, double_post → `NoDoubleCompletion`, lost_on_full → `CqNeverOverfull`, stale_after_teardown → `NoStaleCompletion`). **TLC-green gates Loom-2..6.** | clean 582 distinct states; liveness (`EventuallyCompletes`) 678; each of the 5 buggy cfgs violates exactly its targeted invariant |
| `61ae877` | **Loom-2a** | the ring **substrate**: `kernel/include/thylacine/loom.h` (the ABI — `loom_sqe`/`loom_cqe`/`loom_ring_hdr`/`loom_params` + `_Static_assert`s + `struct Loom`), `kernel/loom.c` (`KObj_Loom`: `loom_create` + the geometry + the refcount + the registered-handle table), `SYS_LOOM_SETUP` (=66) + `SYS_LOOM_REGISTER` (=67) inners + SVC handlers + dispatch, `KOBJ_LOOM` (the fourth partition mask + acquire/release), ARCH §28 I-29/I-30 + §25.4/CLAUDE.md audit-trigger row, `docs/reference/107-loom.md`. The #847 dual-refcount keeps the ring pages alive while the kernel (direct map) OR the user mapping holds a ref. **No op flows yet** — the seam + dispatch are 2b/3. | default 729/729 (+8 `loom.*`) + TCG 729/729 + 0 EXTINCTION; `loom.cfg` re-run green (pre-commit gate) |
| `ac0bda1` + `b857ead` (close) | **Loom-2b** | the **pluggable-completion 9P-engine seam** (audit-bearing): `struct p9_rpc.on_complete` (NULL = sync `WAKE_RENDEZ` / set = async `POST_CQE`); the 3 engine sites (`demux_frame_locked` async dispatch + `map_error` + `on_complete`; `client_mark_dead_locked` async error-CQE; `client_handoff_reader_locked` skips async); `p9_client_submit_async` (no wait, takes ownership); `p9_client_reader_pump_once`; `p9_client_handoff_reader`; `loom_post_cqe` (the CQ writer — `CqNeverOverfull`). Tests in `test_9p_client.c` (the seam, over the loopback) + `test_loom.c` (`loom_post_cqe`). `loom.tla` **unchanged** (`Consume`/`Dispatch`/`ReplyArrives`/`PostCqe` already model it). The production async-op container + the ref-holding / quiesce-before-free lifetime are Loom-3. **Audit closed (one Opus prosecutor 0/0/0/3 + self-audit): self caught SA-1 [P1] the agent missed -- `loom_post_cqe` trusted the shared userspace-writable `cq_mask`/`cq_tail` for its write index (an OOB kernel write in Loom-3); FIXED with a kernel-private `cq_tail` + `l->cq_entries` mask. F2/F3/SA-2 [P3] fixed; F1 [P3] async-op Proc-death quiesce deferred to Loom-3 (#898).** | default + UBSan 735/735 (+6: 3 `9p_client.async*` + `loom.{post_cqe_back_pressure,post_cqe_ignores_hostile_header,dup_rejected}`) under TCG + 0 EXTINCTION; `loom.cfg` clean + liveness + 5 buggy cfgs green |
| *(pending)* (close *(pending)*) | **Loom-3** | the **batch-enter core** (audit-bearing): `SYS_LOOM_ENTER` (=68) + `loom_enter` (SQ-index consume with the kernel-private `sq_head` + the TOCTOU SQE copy + the SQ-index range check); `loom_submit_one` SQE dispatch — `LOOM_OP_NOP` (inline) + `LOOM_OP_FSYNC` (the I-30 submit-time pin: `spoor_ref` + `RIGHT_WRITE` gate + `dev9p_client_fid` → async Tfsync) + reserved/payload opcodes → `-ENOSYS`/`-EINVAL` error CQEs; the production `struct loom_async_op` container (`rpc`@0, the pin, owned by `l->inflight_ops`); `loom_async_complete` (CQE post + terminal under `l->lock`, no sleep); `loom_reap_terminal`; the **`loom_free` quiesce-before-free (#898)** via the new `p9_client_abandon_async` (Tflush + clear `inflight[tag]` under `c->lock`, #845). v1.0 = no-payload opcodes only (payload → Loom-6 registered buffers). `loom.tla` **unchanged** (Loom-3 = `Consume`/`Dispatch`/`Reap`/`Teardown`, already modeled); `9p_client.tla` re-run clean (the engine `abandon_async`). | default + UBSan 741/741 (+6: 3 `loom.enter_*` + 3 `9p_client.loom_*`) under TCG + 0 EXTINCTION; `loom.cfg` clean + liveness + 5 buggy + `9p_client.cfg` clean + 4 buggy green |

## Remaining work (Loom-2..6 — each spec-gated + audited)

- **Loom-2a — ring substrate** (DONE): `KObj_Loom` + the Burrow-backed SQ/CQ
  memory layout + `SYS_LOOM_SETUP` + the registered-handle table
  (`SYS_LOOM_REGISTER`). Self-contained; no 9P-engine change. (A finer split of
  §10's Loom-2 — commit granularity, not deferral: 2a + 2b both land + are
  audited before Loom-3.)
- **Loom-2b — the engine seam** (DONE, **audit-bearing**): the
  pluggable-completion refactor of the #841 client (POST_CQE vs WAKE_RENDEZ on
  the in-flight `p9_rpc`; one engine, two front-ends) + the async submit entry +
  `loom_post_cqe`, landed with its first real tests. **One focused audit over
  2a + 2b is the next step.** The seam touches the audited #841 surface; the
  LOOM completion path has no blocked submitter, so the elected-reader handoff
  skips a LOOM op + a session death completes it with an error CQE (not waking a
  nonexistent rendez). The production async-op container + the ref-holding /
  quiesce-before-free lifetime are Loom-3 (where `SYS_LOOM_ENTER`'s destroy path
  keeps the op off the last-ref-drop-under-lock path).
- **Loom-3 — batch-enter core** (DONE, **audit-bearing**): `SYS_LOOM_ENTER`
  (submit N / reap M) + the SQE → `p9_client_<op>` dispatch (NOP / FSYNC; payload
  opcodes → `-ENOSYS` until Loom-6) + the submit-time pin (LOOM.md §8.5: an
  independent `spoor_ref` + the `RIGHT_WRITE` gate, never re-resolved) + the
  production `loom_async_op` container + the `loom_free` quiesce-before-free
  (#898, via `p9_client_abandon_async` Tflush) + out-of-order completion. **One
  focused audit over Loom-3 is the next step** (it must verify #898 + add the
  deterministic multi-in-flight / Proc-death harness owed since #841).
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
