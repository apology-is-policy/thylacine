# Handoff 039 — P5-audit-r15-c + P5-audit-r15-d (R15 functionally closed)

**Tip**: `c819169` (P5-audit-r15-d hash fixup) on `main`. **R15 audit functionally closed: 9 of 11 findings resolved; only F235 + F236 latent on future RFFDG / SYS_WALK enabling chunks.**

This handoff covers the two R15 closing sub-chunks landed this session. The cumulative R15 prosecutor audit (max-effort Opus-4.7 review since R14 P4-Z) is now functionally closed at the kernel layer — every reachable bug surfaced by the audit has a landed fix. The remaining two findings are preconditions for chunks that don't exist yet (RFFDG-style handle-table sharing; SYS_WALK derived-Spoor lifecycle).

Phase 5 next major chunk: **P5-corvus-bringup** — `/sbin/corvus` Thylacine-native key agent daemon per `docs/CORVUS-DESIGN.md`. All SMP-safety and audit blockers are now closed.

---

## What landed this session (4 commits)

| Commit | Substantive | Hash fixup | Findings closed |
|---|---|---|---|
| **P5-audit-r15-c** | `9641281` | `0288d98` | F230 (P1) + F232 (P2) |
| **P5-audit-r15-d** | `f306529` | `c819169` | F237-F240 (4 × P3) |

### P5-audit-r15-c — 9P client SMP correctness + handles.tla spec extension

**F230 [P1]** — 9P client/session unsynchronized state under cross-Proc dev9p Spoor sharing.

**Fix.** Added `spin_lock_t lock` to `struct p9_client` (`kernel/include/thylacine/9p_client.h`). Every public `p9_client_*` op acquires the lock at entry, releases at exit. Uniform across ~20 functions: handshake / walk / clunk / lopen / lcreate / read / write / getattr / setattr / readdir / statfs / fsync / symlink / mknod / rename / readlink / link / mkdir / renameat / unlinkat / alloc_fid / is_open / inflight / close. The `CLIENT_UNLOCK_RET(c, rc)` helper macro keeps early-return paths terse without converting function bodies to `do-while-0` + `break` structure.

**Lock placement is client-level, not session-level.** The client owns `out_buf` + `next_fid` + counters alongside the embedded session by value. One lock covers all mutable client state. Direct callers of `p9_session_*` below the client (typically test code) self-serialize externally. The 9p_session.h concurrency note was updated to reflect the new structural contract.

**On UP at v1.0 the spin part is a no-op**; SMP race detection awaits TSan enablement (a future P5-tsan-enable chunk). No IRQ-mask variant — 9P client paths are syscall / kthread context only, never IRQ.

**F232 [P2]** — `specs/handles.tla` lacks spec extension for SYS_SPAWN_WITH_FDS / SYS_SPAWN_FULL fd-inheritance.

**Fix.** `specs/handles.tla` extended with:

- **New action `RforkWithFds(parent, child, h, new_rights)`** modeling fd-inheritance. Preconditions: `h ∈ handles[parent]`, `h.kobj ∈ TxKObjs` (no hw kobjs per I-5), `new_rights ⊆ h.rights` (the key guard; F231 closure at the spec level), `new_rights ≠ {}`. Effect: child gains a handle with `via="spawn"`; the `spawn_inherits` ghost ledger records `(child, kobj, parent_rights)` at inheritance time. No session precondition (kernel-internal — same idiom as POSIX exec).
- **New bug class `BuggySpawnFdsElevate`** that skips the rights-subset guard.
- **New ghost VARIABLE `spawn_inherits`** (SUBSET of SpawnInherit records).
- **New invariant `SpawnFdsRightsMonotonic`** cross-checking every "spawn"-via handle's rights against the spawn_inherits ledger. **Distinct from `RightsCeiling`** (which bounds by `origin_rights[k]`); `SpawnFdsRightsMonotonic` bounds by parent's CURRENT rights at fork time — catches the F231 shape (parent had {R}, child got {R,W} where origin had {R,W,T}) that RightsCeiling alone misses.
- **New cfg `handles_buggy_spawn_fds_elevate.cfg`** produces counterexample at depth 4 in <1s.

**State constraint addition.** handles.cfg + all 8 buggy cfgs gained `CONSTRAINT StateConstraint` (Cardinality(handles[p]) ≤ 2 + Cardinality(spawn_inherits) ≤ 2). Required because the new RforkWithFds × HandleDup combinatorics expanded the state space substantially. Clean handles.cfg still verifies the **full state space**: 574M states, 26M distinct, depth 26, 39 min wall time, no invariant violations.

**9p_client.tla companion**. Gained a Concurrency / Lock Discipline header section documenting how the existing buggy variants (TagCollision, OOOMatch, FidAfterClunk, Unbounded) ARE the spec-level coverage of F230's race shapes. The atomic-action modeling style already pins the lock discipline structurally — mirrors `pipe.tla`'s atomic-actions pattern for the pipe-lock + rendez critical section. **No new TLC-checkable actions in 9p_client.tla** — the structural property is already pinned.

**Regression test.** `9p_client.lock_released_between_ops` (kernel/test/test_9p_client.c) verifies `c->lock.value == 0` between every public op (init / handshake / walk / clunk / alloc_fid / is_open / inflight). Two consecutive `alloc_fid` calls assert monotonicity (the spec property: distinct fids under the lock).

### P5-audit-r15-d — R15 P3 polish bundle

**F237 [P3]** — SYS_GETRANDOM partial-fault entropy scrubbing.

Pre-fix: when `uaccess_store_u8` failed mid-stream during the CSPRNG-bytes-to-user copy, the syscall returned -1 but partial entropy lingered in the user buffer — exposing valid CSPRNG bytes that a misguided caller could misuse as known-junk.

Fix: best-effort zero the partial range `[0..i)` + zero the scratch buffer before returning -1. The `uaccess`-of-zero may itself fail (same faulting page); the caller seeing -1 must not trust the buffer regardless.

**F238 [P3]** — dev9p_attach_client release discipline.

Changed `spoor_unref(c)` to `spoor_clunk(c)` in the `priv_alloc`-failure rollback path. Both are correct for a freshly-allocated Spoor (ref=1, dev=&dev9p, aux=NULL) — spoor_clunk runs dev9p_close which safely returns early via `priv_of`'s NULL+magic check. The change is for uniformity: every other Spoor-release path in `kernel/dev9p.c` uses spoor_clunk, and the Plan-9-shape teardown idiom is "close-then-release."

**F239 [P3]** — sys_attach_9p_handler n_uname u64→u32 truncation.

Pre-fix the syscall did `(u32)n_uname` blindly in the `p9_attached_create` call, masking high bits silently. 9P2000.L's n_uname is u32 on the wire.

Fix: added explicit `if (n_uname > (u64)0xFFFFFFFFu) return -1;` guard before the user-VA validation.

**F240 [P3]** — sys_wait_pid_handler status_out partial-write scrubbing.

Pre-fix: on `uaccess_store_u8` failure during the 4-byte int write after the child had been reaped, partial bytes mixed kernel status with pre-existing user data — caller could see torn data.

Fix: best-effort zero the partial range `[0..i)` before returning -1.

**No new regression tests** — all four are degenerate-path patches in already-tested handlers. F237 partial-fault and F240 partial-write paths can only fire on actual uaccess failure (unmapped or RO page), which the kernel test harness doesn't easily simulate. F239 is a guard that fires before any wire activity. F238 is purely structural. The existing test suite (442/442 PASS × default + UBSan) regression-covers the happy paths.

---

## R15 audit final state

| Finding | Sev | Status | Where |
|---|---|---|---|
| F231 | P1 | ✅ Closed | R15-a (`2e7afae` / `b70192e`) |
| F233 | P1 | ✅ Closed | R15-b (`1025026` / `f8f7f22`) |
| F234 | P2 | ✅ Closed | R15-b (`1025026` / `f8f7f22`) |
| F230 | P1 | ✅ Closed | R15-c (`9641281` / `0288d98`) |
| F232 | P2 | ✅ Closed | R15-c (`9641281` / `0288d98`) |
| F237 | P3 | ✅ Closed | R15-d (`f306529` / `c819169`) |
| F238 | P3 | ✅ Closed | R15-d (`f306529` / `c819169`) |
| F239 | P3 | ✅ Closed | R15-d (`f306529` / `c819169`) |
| F240 | P3 | ✅ Closed | R15-d (`f306529` / `c819169`) |
| F235 | P2 | latent | Future RFFDG (handle-table sharing) |
| F236 | P2 | latent | Future SYS_WALK (derived-Spoor lifecycle) |

**Verdict**: R15 met. 9 of 11 closed (all P0/P1/P2 except deferred-on-enabling-chunk; all P3 closed). No active bugs remain. The audit closure file is at `memory/audit_r15_closed_list.md`.

---

## Sanity snapshot at this handoff

- **Tip**: `c819169` on `main`.
- **Tests**: 442/442 PASS × default + UBSan (BOOT_TIMEOUT=60 for UBSan).
- **Specs**: clean handles.cfg verified at full state space (574M states, 26M distinct, depth 26, 39 min). All 9 buggy handles cfgs find counterexamples at depths 3-5 in <1s. 9p_client.cfg + burrow.cfg + scheduler.cfg + pipe.cfg + territory.cfg all clean.
- **Audit closed lists**: R1..R15 cumulative.
- **Phase 5 chunks landed since Phase 5 entry**: P5-spec → P5-wire → P5-session → P5-transport → P5-wire-io → P5-wire-meta → P5-wire-mutation → P5-client → P5-attach-dev → P5-attach-create → P5-attach-mount → P5-spoor-transport → P5-pipe → P5-pipe-blocking → P5-fd-pipe → P5-fd-rw → P5-fd-syscalls → P5-attach-syscall → P5-mount-syscall → P5-attach-probe → P5-corvus-design → P5-corvus-syscalls → P5-joey-from-ramfs → P5-spawn-wait → P5-stratum-api-spec → P5-stratumd-stub-bringup-a → P5-stratumd-stub-bringup-b → P5-spawn-caps → P5-spawn-full → P5-audit-r15-a → P5-audit-r15-b → **P5-audit-r15-c → P5-audit-r15-d**.
- **Working tree**: clean; 176 commits ahead of `origin/main`.

---

## What the next session needs to know

### Read first

1. `CLAUDE.md` (root) — operational framework.
2. `docs/phase5-status.md` — landed chunks table + remaining work.
3. `docs/CORVUS-DESIGN.md` — the next-chunk binding scripture (P5-corvus-bringup).
4. `docs/STRATUM-API-V1.md` — Thylacine-side spec for the 5 Stratum API additions Michal is implementing in parallel.
5. `memory/project_active.md` + `memory/MEMORY.md` — current pickup state.

### Next chunk: P5-corvus-bringup

The next major architectural chunk per `CORVUS-DESIGN.md`. Implements `/sbin/corvus` — the Thylacine-native key agent daemon. Distinct from janus (Stratum's session-secret key agent): corvus runs as a long-lived userspace Proc, holds DEKs mlocked in RAM (CAP_LOCK_PAGES), generates session tokens for stratumd binding (CAP_CSPRNG_READ), supports HKDF unwrap of pool-bound keys with pool_serial binding (C-14), and tracks login session lifecycle for DEK eviction.

**Why now**: SMP-safety blocker (F230 R15-c) is closed; dev9p Spoor inheritance via SYS_SPAWN_WITH_FDS is now race-free; corvus can be safely shared across Procs. The R15 audit is also functionally closed — no audit debt to carry into the new chunk.

**Preconditions all met**:
- ✅ SYS_SPAWN_FULL (= 25) — joey can spawn corvus with pipe-pair + CAP_LOCK_PAGES + CAP_CSPRNG_READ in one call.
- ✅ Corvus hardening syscalls (SYS_MLOCKALL, SYS_SET_DUMPABLE, SYS_SET_TRACEABLE, SYS_EXPLICIT_BZERO, SYS_GETRANDOM) — landed at P5-corvus-syscalls.
- ✅ 9P client SMP-safe — R15-c closure.
- ✅ Atomic Spoor + pipe refcounts — R15-b closure.
- ✅ Rights-monotonic spawn-fd inheritance — R15-a closure.

**External dependency**: Stratum-side STRATUM-API-V1.md asks (A1 pool serial, A2 multi-stratumd, A3 corvus UNWRAP wire, A4 corvus notify Spoor / DEK eviction, A5 rollback marker) — Michal is implementing in parallel. Thylacine-side corvus impl can develop against the spec'd wire format; integration testing waits for Stratum-side landing.

**Alternative next chunks** (smaller, can interleave):
- **P5-stratumd-stub-c**: long-running stub serving multiple sequential clients — small; extends the existing stub-driver.
- **P5-stratumd-stub-d**: pivot/chroot kernel mechanism — substantial; gated on string-path walk subsystem; arguably the biggest remaining v1.0 piece.

### Stratum coordination

Stratum v2 is feature-complete and shipping per `stratum/v2/docs/OS-INTEGRATION.md`. The 5 API additions enumerated in `docs/STRATUM-API-V1.md` are Michal-owned and tracked in parallel. Thylacine's Phase 5 binds to Stratum's stable 9P2000.L wire + libstratum-9p ABIs.

### Composition-layer discipline carried forward

- **R12 carryovers**: every audit-trigger surface introduced in Phase 4 carried R12-* deferred-audit closures into Phase 5 (already closed). Future virtio-class drivers must use `libthyla_rs::virtio_rmb()` (R14 closure; ARCH §28 I-9 corollary).
- **R15 carryovers**: 9P client SMP discipline (client-level spin_lock); atomic Spoor / pipe refcounts (ACQ_REL ordering + pre-value capture); spawn-fd inheritance preserves parent's rights (rights array captured in spawn_with_fds_args).
- **F235 / F236 deferred carryovers**: when RFFDG / SYS_WALK chunks land, audit must check that the handle-table lock + derived-Spoor lifecycle protect against the racy / UAF shapes the audit flagged.

### Memory + status discipline

- `memory/audit_r15_closed_list.md` is the canonical R15 record; the do-not-re-report preamble for the future R16 prosecutor.
- `docs/phase5-status.md` has rows for every R15-* sub-chunk (entries `*(pending)*` → hash on fixup).
- `docs/REFERENCE.md`'s Tip line tracks the latest substantive + hash-fixup.

---

## The thylacine substrate at this handoff

- **Syscall surface**: 25 syscalls. Read / write / pipe / fd / attach / mount / spawn (4 variants) / wait_pid / corvus hardening (5) / handle / mmio / dma / irq / putc / puts / exits. Audited cumulative across R1..R15.
- **Userspace orchestration**: `/joey` (real userspace binary from initrd) spawns `/hello` and `/stub-driver`; `/stub-driver` drives the full production-shape orchestration at EL0 (pipe × 2 + spawn-with-fds + attach + mount + unmount + close + wait).
- **9P client**: full 9P2000.L surface (handshake / walk / IO / metadata / mutation) over a per-client lock, transport-agnostic, dev9p-routable. SMP-safe at v1.0 (UP no-op spin part; real LSE atomics for refcount sites).
- **Spec coverage**: 4 of 9 mandatory specs landed + the 9P client spec from Phase 5 entry (= 5 of 9; remaining 4 are poll / futex / notes / pty for Phase 5+).
- **Audit posture**: R1..R15 cumulative; 100% of P0/P1/P2 findings reachable at v1.0 closed; deferred findings tracked as preconditions for the chunks that enable them.

---

**The thylacine runs again** — at R15 audit close, with a SMP-safe 9P client, on the precipice of P5-corvus-bringup.
