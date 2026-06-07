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
seam), Loom-3 (the batch-enter core — `SYS_LOOM_ENTER` + SQE dispatch + the
I-30 submit-time pin + the async-op container + the `loom_free` quiesce), and
Loom-4a/4b/4c (the SQPOLL arc — the transport deadline + deadline-aware pump, the
CQ wait-list, and the SQPOLL poll-thread + `SYS_LOOM_SETUP(LOOM_SETUP_SQPOLL)` +
the stop/join lifetime), and Loom-4d (the focused SQPOLL audit + close) are
COMPLETE. The Loom-5 SPEC (the multishot + LINK/DRAIN models, two NEW focused
modules `specs/loom_multishot.tla` + `specs/loom_order.tla`) is landed, and
**Loom-5a (the multishot IMPL)** wires the stream mechanism — one
`LOOM_SQE_MULTISHOT` SQE → a `LOOM_CQE_MORE` shot per reply that re-arms the op +
one MORE-clear terminal — against the synthetic FSYNC vehicle, and **Loom-5b
(the LINK/DRAIN IMPL)** wires the inter-op ordering — an ordering-relevant SQE is
HELD in a per-ring chain (`l->chain`) and dispatched only once its gates open (a
linked successor after its predecessor is done ok; a failed link member cancels
the rest of the chain, each posting one `-ECANCELED` CQE; a drain barrier after
all prior ops are done) — also against the synthetic NOP/FSYNC vehicles. The
**Loom-5 focused audit is CLOSED** (one Opus prosecutor + self-audit; 0 P0 / 0 P1
/ 2 P2 / 4 P3 — mechanism SOUND; F1 drain-vs-rearm-pending + F2 cancel-CQE-drop +
F3 lock-free-state-write FIXED; F4/F5/F6 concurrency-contract + OOM-LINK +
LINK-single-producer DOCUMENTED; the F2 dispatch-leg over-admit residual + the
exact-concurrent-admission coordination OWED to Loom-6 with the SMP harness). Next
is **Loom-6** (registered buffers + the native libthyla-rs API + the bench).** v1.0
dispatches the no-payload opcodes (NOP / FSYNC); the payload opcodes land with
Loom-6's registered-buffer surface.

## Landed sub-chunks

| Commit | Sub-chunk | What | Verification |
|---|---|---|---|
| `ebd1f7b` | **Loom-0** | scripture — `docs/LOOM.md` + the NOVEL.md promotion + the ROADMAP §8.0a/§12.2 registration. No code. | (docs only) |
| `7983794` | **Loom-1** | `specs/loom.tla` — the SQ/CQ op-lifecycle state machine + the cfg matrix. Pins **I-29** (completion integrity: no-lost / no-double / no-stale) + **I-30** (submit-time capability pin) + ring TOCTOU + CQ back-pressure. 12 safety invariants + 1 liveness property; clean cfg + a liveness cfg + 5 buggy-cfg counterexamples (live_sqe_reread → `ArgPinnedToSnapshot`, recheck_at_completion → `ObjPinnedToSnapshot`, double_post → `NoDoubleCompletion`, lost_on_full → `CqNeverOverfull`, stale_after_teardown → `NoStaleCompletion`). **TLC-green gates Loom-2..6.** | clean 582 distinct states; liveness (`EventuallyCompletes`) 678; each of the 5 buggy cfgs violates exactly its targeted invariant |
| `61ae877` | **Loom-2a** | the ring **substrate**: `kernel/include/thylacine/loom.h` (the ABI — `loom_sqe`/`loom_cqe`/`loom_ring_hdr`/`loom_params` + `_Static_assert`s + `struct Loom`), `kernel/loom.c` (`KObj_Loom`: `loom_create` + the geometry + the refcount + the registered-handle table), `SYS_LOOM_SETUP` (=66) + `SYS_LOOM_REGISTER` (=67) inners + SVC handlers + dispatch, `KOBJ_LOOM` (the fourth partition mask + acquire/release), ARCH §28 I-29/I-30 + §25.4/CLAUDE.md audit-trigger row, `docs/reference/107-loom.md`. The #847 dual-refcount keeps the ring pages alive while the kernel (direct map) OR the user mapping holds a ref. **No op flows yet** — the seam + dispatch are 2b/3. | default 729/729 (+8 `loom.*`) + TCG 729/729 + 0 EXTINCTION; `loom.cfg` re-run green (pre-commit gate) |
| `ac0bda1` + `b857ead` (close) | **Loom-2b** | the **pluggable-completion 9P-engine seam** (audit-bearing): `struct p9_rpc.on_complete` (NULL = sync `WAKE_RENDEZ` / set = async `POST_CQE`); the 3 engine sites (`demux_frame_locked` async dispatch + `map_error` + `on_complete`; `client_mark_dead_locked` async error-CQE; `client_handoff_reader_locked` skips async); `p9_client_submit_async` (no wait, takes ownership); `p9_client_reader_pump_once`; `p9_client_handoff_reader`; `loom_post_cqe` (the CQ writer — `CqNeverOverfull`). Tests in `test_9p_client.c` (the seam, over the loopback) + `test_loom.c` (`loom_post_cqe`). `loom.tla` **unchanged** (`Consume`/`Dispatch`/`ReplyArrives`/`PostCqe` already model it). The production async-op container + the ref-holding / quiesce-before-free lifetime are Loom-3. **Audit closed (one Opus prosecutor 0/0/0/3 + self-audit): self caught SA-1 [P1] the agent missed -- `loom_post_cqe` trusted the shared userspace-writable `cq_mask`/`cq_tail` for its write index (an OOB kernel write in Loom-3); FIXED with a kernel-private `cq_tail` + `l->cq_entries` mask. F2/F3/SA-2 [P3] fixed; F1 [P3] async-op Proc-death quiesce deferred to Loom-3 (#898).** | default + UBSan 735/735 (+6: 3 `9p_client.async*` + `loom.{post_cqe_back_pressure,post_cqe_ignores_hostile_header,dup_rejected}`) under TCG + 0 EXTINCTION; `loom.cfg` clean + liveness + 5 buggy cfgs green |
| `512a6c4` + `5ee2227` (close) | **Loom-3** | the **batch-enter core** (audit-bearing): `SYS_LOOM_ENTER` (=68) + `loom_enter` (SQ-index consume under `l->lock` with the kernel-private `sq_head` + the submit-time CQ admission + the TOCTOU SQE copy + the SQ-index range check); `loom_submit_one` SQE dispatch — `LOOM_OP_NOP` (inline) + `LOOM_OP_FSYNC` (the I-30 submit-time pin: `spoor_ref` + `RIGHT_WRITE` gate + `dev9p_client_fid` → async Tfsync) + reserved/payload opcodes → `-ENOSYS`/`-EINVAL` error CQEs; the production `struct loom_async_op` container (`rpc`@0, the pin, owned by `l->inflight_ops`); `loom_async_complete` (CQE post + terminal under `l->lock`, no sleep); `loom_reap_terminal`; the **`loom_free` quiesce-before-free (#898)** via the new `p9_client_abandon_async` (Tflush + clear `inflight[tag]` under `c->lock`, #845). v1.0 = no-payload opcodes only (payload → Loom-6 registered buffers). `loom.tla` **unchanged** (Loom-3 = `Consume`/`Dispatch`/`Reap`/`Teardown`, already modeled); `9p_client.tla` re-run clean (the engine `abandon_async`). | default + UBSan 742/742 (+7: 4 `loom.enter_*` + 3 `9p_client.loom_*`) under TCG + 0 EXTINCTION; `loom.cfg` clean + liveness + 5 buggy + `9p_client.cfg` clean + 4 buggy green. **Audit: 2 rounds CLEAN** -- round-1 (Opus prosecutor + self-audit) 0/1/1/3 all fixed (F1 submit-phase `sq_head` SMP race; F2 CQ-admission lost-completion; F3 `loom_free` lock; F4 pump cap; F5 rename), round-2 on the fixes (dirty-close discipline) 0/0/0/0 |
| `1ffba08` | **Loom-4a** | the **SQPOLL transport-deadline substrate**: the two NULL-permitted `p9_transport_ops` `set_recv_deadline` / `recv_timed_out` (+ NULL-safe shims) wired into srvconn (`srvconn_set_client_deadline` / `srvconn_client_timed_out`), the loopback test backend (a deterministic frame-boundary-timeout knob), spoor (NULL); `reader_recv_frame(deadline_ns, idle)` arms the deadline on ONLY the first recv (the frame boundary; a mid-frame deadline would desync, #841) and reads `recv_timed_out` BEFORE disarming; `p9_client_reader_pump_once_deadline` + `enum p9_pump_result {DEAD,IDLE,PROGRESS,BUSY}` where IDLE leaves the session alive+synced. `loom.tla` **unchanged** (4a is transport plumbing below the CQ-waiter model). | default 748/748 (+6: 4 `9p_client.pump_deadline_*` + `9p_transport.deadline_idle_vs_eof` + `9p_srvconn_transport.deadline_vtable_routes`) under TCG + 0 EXTINCTION; `loom.cfg` gate green |
| `9a2b2e4` | **Loom-4b** | the **CQ wait-list** on `struct Loom` + the `loom_enter` wait-phase rework (resolves the Loom-3 concurrent-ENTER limitation): a `struct poll_waiter_list cq_waiters` (Rendez is single-waiter); `loom_post_cqe` wakes it after publishing a CQE (the pipe.c release-then-wake; under `c->lock` from `loom_async_complete` — `poll_waiter_list_wake` does not sleep / re-enter `p9_client_*`, the seam contract); `loom_wait_for_completions` either DRIVES the elected reader itself (the Loom-3 path) or — when a sibling thread of the SAME Proc holds the reader role — sleeps on `cq_waiters` until that reader posts a CQE (register-then-observe, poll.tla lineage; death-interruptible #811; flood-bounded). NoStrandedWaiter holds vacuously now (a `loom_enter` caller holds a loom ref, so `loom_free` cannot run while a waiter sleeps — KObj_Loom is per-Proc + non-transferable, so all concurrent ENTERs are sibling threads that group-terminate together). `loom.tla` **unchanged** (4b implements the `d48a8da` CQ-waiter model: `CqWaitRegister` / `PostCqe`-wake / `CqWaitCommitOrSleep`). | default 752/752 (+4: `loom.cq_waiter_wake` / `loom.cq_waiter_no_spurious_wake_on_full` / `loom.enter_inline_min_complete` / `loom.enter_min_complete_no_inflight`) under TCG + 0 EXTINCTION; `loom.cfg` clean (2429) + liveness (1457) + all 7 buggy cfgs violate their target; SMP gate (default-smp4 + ubsan-smp4 N=10) 0 corruption |
| `d043f64` | **Loom-4c** | the **SQPOLL poll-thread** + `SYS_LOOM_SETUP(LOOM_SETUP_SQPOLL)` + the stop/join lifetime: a per-ring `kproc()` kthread (`loom_sqpoll_main`, the `console_mgr` precedent) drains the SQ zero-syscall (`loom_drain_sq`, factored out of `loom_enter`) + drives the elected reader with a `timer_now_ns()+10ms` frame-boundary idle-deadline (the 4a `_deadline` pump; IDLE re-checks the stop flag, #841-safe); parks on a new `sqpoll_park` Rendez (lock-free cond `stopping \|\| sq_tail!=sq_head`) when idle, announcing `LOOM_RING_SQ_NEED_WAKEUP`. `struct Loom += sqpoll`/`sqpoll_stopping`/`sqpoll_exited`/`sqpoll_park`. `loom_free` JOINS it FIRST (set stopping + wake park + spin `sqpoll_exited` + `thread_free`) before the #898 quiesce — the kthread holds NO loom ref, the join is the lifetime authority. The terminal handshake masks IRQs (`spin_lock_irqsave(NULL)`, the idle-loop idiom) across `state=EXITING` + the `sqpoll_exited` release so the joiner observes EXITING before `thread_free` (the wait_pid reap minus the kproc-forbidden zombie bookkeeping). `loom_enter` on an SQPOLL ring does NOT submit (the kthread owns it) -- it wakes the park. The new `p9_client_recv_is_deadline_capable` gates `loom_register_handles`: a NULL-deadline dev9p client cannot register into an SQPOLL ring (so the kthread's recv is always interruptible -> the join always terminates). `loom.tla` **unchanged** (4c reuses the `d48a8da` CQ-waiter + Teardown model; the kthread is one PostCqe producer + the Teardown actor). | default 754/754 (+2: `loom.sqpoll_setup_and_teardown` / `loom.sqpoll_drains_sq`) under TCG + 0 EXTINCTION; `loom.cfg` clean (2429) + liveness (1457) + all 7 buggy cfgs violate their target; SMP gate (default-smp4 + ubsan-smp4 N=10) 0 corruption |
| `9035df7` | **Loom-4d** | the **focused SQPOLL audit + close** (the whole Loom-4 surface: 4a transport-deadline + 4b CQ-waitlist + 4c kthread). One Opus prosecutor (0 P0 / 1 P1 / 1 P2 / 3 P3) + a concurrent self-audit; the EXITING terminal, the join-vs-mid-recv wake, the deadline gate, the lock-free park cond, the factored drain, and the CQ wait-list all VERIFIED SOUND. **Fixes**: **F1 [P1]** the `loom_first_inflight_client` borrowed-client UAF (a concurrent reap + re-register could free the Spoor -> the `p9_client` while the kthread/ENTER held the bare `cl`) -> the helper now takes a borrow-guard `spoor_ref` on the op's pin, returned + clunked after the pump; **F2 [P2]** the SQPOLL park busy-loop on CQ-full backpressure -> `loom_sqpoll_park_cond` now gates on CQ admittability (`stopping \|\| (sq_tail!=sq_head && loom_cq_ready < cq_entries)`), race-free because async_inflight==0 in the park branch makes the kthread the sole CQ producer; **SA-2 [P3]** the transient reader-contention busy-loop -> `sched()` yield on `P9_PUMP_BUSY`; **F3 [P3]** the NULL-deadline transport mis-named (it's the spoor pipe-pair, not the loopback) -> comments corrected; **F5 [P3]** the test `cq_tail` read -> the shared-header mirror. **F4 [P3]** mid-frame-recv-unbounded -> v1.x untrusted-server seam (documented). DIRTY close (the F2 fix changes the park wait/wake predicate) -> **CONVERGED CLEAN over 2 rounds** (the Loom-3 precedent): round-2 prosecuted the fixes (Opus + self-re-audit) 0 P0 / 0 P1 / 0 P2 / 1 P3 -- F1/F2/SA-2/the test all VERIFIED SOUND; the lone **R2-F1 [P3]** (`loom_first_inflight_client` `*pin_out` not NULL'd on the not-found path -- a latent footgun for future callers) FIXED `@6d85bfc` (`*pin_out = NULL`, behaviorally inert). `loom.tla` **unchanged** (no new mechanism). The ARCH §25.4 / CLAUDE.md Loom audit-trigger row gains the SQPOLL kthread + the join + the deadline gate + the F1/F2 surface. | default 755/755 (+1: `loom.sqpoll_parks_on_cq_full`, the F2 regression) under TCG + 0 EXTINCTION; `loom.cfg` clean (2429) + liveness (1457) + all 7 buggy cfgs violate; SMP gate (default-smp4 + ubsan-smp4 N=10) 0 corruption |
| `c0e555a` | **Loom-5 (spec)** | the **multishot + LINK/DRAIN models** (spec-first, no impl): two NEW focused modules, leaving the audited `loom.tla` untouched (its single-CQE-per-op `cq \subseteq Ops` is gate-tied and cannot represent a multiset CQ — refactoring it would invalidate its 8 landed cfgs; the `sched_oncpu.tla` + `sched_alpha.tla` precedent). **`specs/loom_multishot.tla`** — the stream lifecycle (arm → MORE shots → exactly-one terminal): I-29 GENERALIZED to a stream (`ExactlyOneTerminal` + `TerminalEndsStream`), the I-30 pin held ACROSS shots (`ObjPinnedAcrossShots` — the clunk+reuse-between-shots amplification), per-shot back-pressure (`CqNeverOverfull`, a COUNT CQ), teardown-quiesce (`NoStaleAfterTeardown`); clean + liveness (`EventuallyTerminal`) + 5 buggy cfgs. **`specs/loom_order.tla`** — LINK/DRAIN admission ordering over a chain (`Pred(i)` predecessor set, domain-safe): `LinkOrdered` + `DrainOrdered` + `EveryDoneOpPosted` (a cancelled linked op posts exactly one CQE, never dropped) + `NoOrphanCancel` + the liveness `EverySubmittedPosts`; clean + liveness + 4 buggy cfgs. SPEC-TO-CODE.md + LOOM.md §7/§9/§10 updated. The Loom-5 impl (#909) wires the mechanism against synthetic NOP/FSYNC vehicles. | `loom_multishot.cfg` clean (2940) + liveness (1633) + 5 buggy cfgs violate their target; `loom_order.cfg` clean (1505) + liveness + 4 buggy cfgs violate; the whole Loom suite (3 modules, 22 cfgs) green |
| `3cf1852` | **Loom-5b (LINK/DRAIN impl)** | the **LINK/DRAIN inter-op ordering mechanism** (`specs/loom_order.tla`) wired against the synthetic NOP/FSYNC vehicles. A per-ring **held-submission chain** (`l->chain`, an ordered list under `l->lock`) layered ON TOP of the audited `loom_async_op` lifecycle (a chain entry `struct loom_chain_op` = a kernel SQE copy + `link`/`drain` flags + `state`): `loom_drain_sq` routes an ordering-relevant SQE (LINK/DRAIN set, or any SQE while the chain is non-empty) into the chain HELD instead of dispatching it; `loom_admit_chain` (in the drive loops, OUTSIDE `c->lock` — the seam contract) walks head→tail and dispatches each entry once its gates open: the **link cancel-cascade** (a HELD op whose linked predecessor finished non-ok is CANCELLED with exactly one `-ECANCELED` CQE, never dispatched — `EveryDoneOpPosted` + `NoOrphanCancel`), the **link gate** (`LinkAdmits` — a linked successor waits for predecessor `DONE_OK`), the **drain gates** (`DrainAdmits` — a post-drain op behind every earlier drain; a drain itself behind every chain-before op AND `async_inflight==0`, the catch for prior FAST async ops). Each action takes one CQ-admission slot (`loom_cq_ready + async_inflight < cq_entries`), no room HOLDS the chain (back-pressure, like `loom_rearm_pending`); the SQPOLL park cond wakes on a non-empty chain so a CQ-back-pressured held op resumes. `loom_reclaim_chain` frees terminal entries only when NO entry is HELD (a HELD entry may still read a predecessor's result); the chain is bounded at `cq_entries`. A chain member is never multishot (combo rejected `-EINVAL`); `CQE_SKIP` is rejected (deferred — needs a `loom_order.tla` carve-out). New errno **`T_E_CANCELED` (=125, POSIX ECANCELED)** for the cancel CQE. | default 761/761 + UBSan 761/761 (+4: `9p_client.loom_{link_cancel_cascade,link_success_ordering,drain_barrier,independent_past_held}`; the pre-existing `loom.enter_flags_and_bad_index` flag-reject moved to `CQE_SKIP`) under TCG + 0 EXTINCTION; `loom_order.cfg` clean (1505) + liveness + 4 buggy cfgs violate; `loom.tla` (2429) + `loom_multishot.cfg` (2940) re-run clean; whole Loom suite (3 modules / 22 cfgs) behaving |
| *(pending)* | **Loom-5 audit close** (whole 5a + 5b surface) | one **Opus prosecutor + a concurrent self-audit**; **0 P0 / 0 P1 / 2 P2 / 4 P3** -- the mechanism VERIFIED SOUND + memory-safe even under concurrent drivers (the INFLIGHT claim under `l->lock`). The self-audit independently found **F1 (cross-confirmed)**. All 6 share ONE root (F4): the chain scheduler reasons single-drainer; the substrate supports concurrent non-SQPOLL ENTERs. **F1 [P2] FIXED** -- the drain-self gate ignored `rearm_pending`, so a later DRAIN could admit early past a back-pressured FAST multishot stream (`DrainOrdered`); gate is now `async_inflight==0 && rearm_pending==0` + a deterministic regression `9p_client.loom_drain_waits_for_rearm_pending`. **F2 [P2] cancel leg FIXED** (revert-to-HELD + retry on a dropped `-ECANCELED` post; the dispatch-leg over-admit = the inherited Loom-3 residual -> OWED to Loom-6). **F3 [P3] FIXED** (`loom_chain_done` writes `chain->state` under `l->lock`). **F4/F5/F6 [P3] DOCUMENTED** (single-driver-per-ring contract + OOM-degrades-LINK + LINK-group single-producer -- 107-loom.md "Known caveats"). NOT a dirty close (0 P0/P1; localized gate/lock/retry fixes). `memory/audit_loom_closed_list.md` Loom-5 section; ARCH 25.4 / CLAUDE.md audit-trigger row updated. | default **762/762** + UBSan **762/762** (+1 regression) under TCG + 0 EXTINCTION; the 3-module spec suite (22 cfgs) green; **SMP gate (default-smp4 + ubsan-smp4 N=10) 0 corruption** |
| `cbba808` | **Loom-5a (multishot impl)** | the **multishot mechanism** (`specs/loom_multishot.tla`) wired against the synthetic FSYNC vehicle. `loom_submit_one` accepts `LOOM_SQE_MULTISHOT` + sets `op->{build,multishot,shot_limit}` (shot_limit in `sqe->offset`); the I-30 pin + tag taken ONCE at submit, never re-resolved per shot (`ObjPinnedAcrossShots`). `loom_async_complete` (UNDER `c->lock`) decides MORE-vs-terminal, posts the CQE (`LOOM_CQE_MORE` set on a shot / clear on the terminal), then under `l->lock` either flags `op->rearm` (+ `rearm_pending++`) or `op->terminal`. The new `loom_rearm_pending` re-issues `op->build` in BOTH drive loops (`loom_wait_for_completions` + `loom_sqpoll_main`, OUTSIDE `c->lock` — the seam contract), reserving one CQ slot per shot (`CqNeverOverfull`); a back-pressured shot is HELD, never dropped. The SQPOLL park cond also wakes on `rearm_pending > 0 && CQ-room` so a post-reap ENTER resumes a held stream. Teardown's #898 quiesce already covers a re-armable op. An inline NOP ignores MULTISHOT (async-only mechanism). | default 757/757 + UBSan 757/757 (+2: `9p_client.loom_multishot_{stream,backpressure}`) under TCG + 0 EXTINCTION; `loom.tla` clean (2429) + `loom_multishot.cfg` clean (2940) + liveness (1633) + 5 buggy cfgs violate; whole Loom suite (3 modules / 22 cfgs) behaving |

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
- **Loom-4 — SQPOLL** (**design signed off + `loom.tla` extended**; impl in
  progress, split 4a..4d): the `kproc()` poll-thread (zero-syscall hot path;
  `cpu_pinned`-able) + the frame-boundary idle-deadline (the #841-safe
  interruptible recv; a new NULL-permitted transport `set_recv_deadline` + a
  deadline-aware reader pump) + the CQ wait-list (an `ENTER` waiter sleeps for
  `min_complete`, woken by a posted CQE / teardown). Option 1 of the user-voted
  design fork (LOOM.md §8.6); the owning-Proc-member-thread + submission-only
  alternatives were rejected. `loom.tla` extended FIRST (the CQ-waiter actor:
  `CqFlagTracksCq` + `NoMissedCqWake` + `NoStrandedWaiter` + `CqWaiterReturns`;
  +2 buggy cfgs `cqwait_no_wake` / `cqwait_check_early`; clean 2429 states, all 9
  cfgs green). Wait/wake + kthread-lifetime surface. Audit.
  - **Loom-4a — transport deadline + deadline-aware pump** (DONE): the two
    NULL-permitted `p9_transport_ops` ops `set_recv_deadline` / `recv_timed_out`
    (+ NULL-safe shims) wired into srvconn (`srvconn_set_client_deadline` /
    `srvconn_client_timed_out`), the loopback test backend (a deterministic
    frame-boundary-timeout knob), and spoor (NULL); `reader_recv_frame` arms the
    deadline on ONLY the first recv (frame boundary) and disarms mid-frame;
    `p9_client_reader_pump_once_deadline` + `enum p9_pump_result`
    (`DEAD`/`IDLE`/`PROGRESS`/`BUSY`). 6 tests (4 `9p_client.pump_deadline_*` + 1
    `9p_transport.deadline_idle_vs_eof` + 1 `9p_srvconn_transport.deadline_vtable_routes`).
    `loom.tla` unchanged (4a is transport plumbing below the CQ-waiter model).
  - **Loom-4b** — the CQ wait-list on `struct Loom` + `loom_enter` rework (DONE):
    `struct poll_waiter_list cq_waiters` + `loom_post_cqe` wakes it (release-then-
    wake; safe under the async path's `c->lock`) + `loom_wait_for_completions`
    (drive-the-reader OR sleep-as-a-CQ-waiter; register-then-observe; death-
    interruptible #811; flood-bounded). Resolves the Loom-3 "concurrent ENTER
    returns what's posted" limitation. `loom.tla` unchanged (implements the
    `d48a8da` CQ-waiter model). The full sleep -> woken-by-a-real-sibling-reader
    interleaving is the deferred multi-in-flight / cross-Proc-death SMP harness
    (OWED since #841; lands with 4d).
  - **Loom-4c** — the SQPOLL kthread + `SYS_LOOM_SETUP(LOOM_SETUP_SQPOLL)` +
    the stop/join lifetime (DONE): the per-ring `kproc()` kthread
    (`loom_sqpoll_main`) drains the SQ zero-syscall (`loom_drain_sq`) + drives the
    reader with a frame-boundary idle-deadline (the 4a `_deadline` pump) + parks on
    `sqpoll_park` when idle; `loom_free` JOINS it (stop + wake + spin `sqpoll_exited`
    + `thread_free`) before the #898 quiesce; the IRQ-masked EXITING terminal
    handshake (the kproc thread cannot use `thread_exit_self`); `loom_register_handles`
    gates a NULL-deadline dev9p client out of an SQPOLL ring (so the join always
    terminates). `loom.tla` unchanged (4c reuses the `d48a8da` model). The full
    kthread-vs-ENTER-vs-Proc-death interleaving over a live client is the OWED 4d
    SMP harness.
  - **Loom-4d** — the focused audit (kthread lifetime + CQ wait/wake) + close
    (DONE): one Opus prosecutor (0/1/1/3) + a concurrent self-audit; the core
    cruxes (the EXITING terminal, the join, the deadline gate, the lock-free park
    cond, the factored drain, the CQ wait-list) VERIFIED SOUND. Fixes: F1 [P1] the
    `loom_first_inflight_client` borrowed-client UAF (borrow-guard `spoor_ref`); F2
    [P2] the CQ-full park busy-loop (the cond gates on admittability) + regression
    `loom.sqpoll_parks_on_cq_full`; SA-2 [P3] yield on `P9_PUMP_BUSY`; F3 [P3]
    NULL-deadline-transport comment (spoor, not loopback); F5 [P3] the test
    `cq_tail` mirror read; F4 [P3] mid-frame-recv-unbounded -> v1.x untrusted-server
    seam. DIRTY close (F2 changes the park wait/wake predicate) -> CONVERGED CLEAN
    over 2 rounds: round-2 on the fixes 0/0/0/1 (R2-F1 [P3] out-param-NULL hardening
    fixed @6d85bfc). The ARCH §25.4/CLAUDE.md Loom row gains the SQPOLL surface. The
    deterministic two-thread-same-loom_fd + cross-Proc-death + live-FSYNC SMP
    harness is **STILL OWED** (the 4c/4d tests are NOP-only; F1 is reasoned +
    round-2-audited, not test-reproduced) -- needs dev9p-loopback-in-loom infra +
    real SMP + restored TSan; carried to Loom-6 + #841/#907.
- **Loom-5 — the multishot MECHANISM + linked ops**: one SQE → many CQEs
  (re-arm + `LOOM_CQE_MORE` + terminal-exactly-once + back-pressure-holds-a-shot
  + cancel — the I-29 generalization to a stream) + LINK/DRAIN per-fid ordering.
  Its real consumers (the **Tapestry** event-fd stream + the `/srv` accept-loop —
  the same mechanism) are payload ops that land at Loom-6, so the mechanism is
  modeled FIRST (the **SPEC is LANDED**: the two new focused modules
  `loom_multishot.tla` + `loom_order.tla`, leaving the audited `loom.tla`
  untouched — its single-CQE-per-op `cq` can't hold a multiset) + then built +
  audited against **synthetic NOP/FSYNC multishot vehicles** (user-voted
  2026-06-07 "mechanism at Loom-5, real ops at Loom-6"). The IMPL (#909) is next:
  Audit.
- **Loom-6 — registered buffers + payload ops + native API + bench**: pinned
  Burrow regions (zero-copy payload) + the real payload-op dispatch (the event-fd
  multishot `LOOM_OP_READ` + the present `LOOM_OP_WRITE` + READ/WRITE/WALK/…) the
  Loom-5 mechanism rides + the libthyla-rs Loom wrapper (the native userspace API
  — the backend the libtapestry `Loom` seam trait targets) + a latency benchmark
  on a high-fanout workload (the **Tapestry** present+input+vsync loop; globbing /
  many-connection server). The `LOOM_OP_WIRE_PASSTHROUGH` seam stays reserved
  (designed, not built). Final audit + arc close.

**Consumer (shapes Loom-5/6):** Tapestry, the graphics fast-path
(`docs/TAPESTRY.md`, signed off 2026-06-07) — present = `LOOM_OP_WRITE`,
input/vsync = multishot `LOOM_OP_READ`, the framebuffer a zero-copy shared Burrow
(host DMA-read out of band, not a Loom regbuf). Adds the **T-1 no-torn-scanout**
graphics-layer invariant (the #847 dual-refcount + #898 quiesce are the
mechanism), audited at the post-Loom graphics phase (virtio-gpu scanout +
`tapestryd`), not in the Loom arc. A native POC compiles on the aux track
(`usr/apps/libtapestry` + `usr/apps/tapestry-demo`).

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
# the Loom spec SUITE is three modules (core + Loom-5 multishot + Loom-5 order).
# core loom.tla — clean (15 safety invariants) + liveness + 7 buggy cfgs:
java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock -config loom.cfg loom.tla
java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock -config loom_liveness.cfg loom.tla
for b in live_sqe_reread recheck_at_completion double_post lost_on_full stale_after_teardown cqwait_no_wake cqwait_check_early; do
  java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock -config "loom_buggy_${b}.cfg" loom.tla
done
# Loom-5 multishot (clean + liveness + 5 buggy):
java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock -config loom_multishot.cfg loom_multishot.tla
java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock -config loom_multishot_liveness.cfg loom_multishot.tla
for b in double_terminal shot_lost_on_full resolve_at_shot more_after_terminal stale_after_teardown; do
  java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock -config "loom_multishot_buggy_${b}.cfg" loom_multishot.tla
done
# Loom-5 LINK/DRAIN order (clean + liveness + 4 buggy):
java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock -config loom_order.cfg loom_order.tla
java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock -config loom_order_liveness.cfg loom_order.tla
for b in link_reorder drain_jumps_ahead cancel_no_cqe cancel_skips; do
  java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock -config "loom_order_buggy_${b}.cfg" loom_order.tla
done
```

## Trip hazards (carry into Loom-2..6)

- **Spec-first is re-enabled for this surface.** A TLC-green Loom spec SUITE is a
  pre-commit gate for every Loom impl sub-chunk; a new mechanism extends the suite
  FIRST, then the impl (docs/LOOM.md §7 + §10). Loom-2 added the engine front-end +
  setup actions to `loom.tla`; Loom-4 the SQPOLL wait/wake (the CQ-waiter actor);
  Loom-5 added TWO NEW focused modules — `loom_multishot.tla` (the multiset-CQ
  stream lifecycle) + `loom_order.tla` (LINK/DRAIN) — rather than refactor the
  audited core (whose single-CQE-per-op `cq` is gate-tied), the `sched_oncpu.tla` +
  `sched_alpha.tla` precedent. The gate is now all three modules (22 cfgs).
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
