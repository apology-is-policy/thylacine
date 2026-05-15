# 65. /sbin/corvus skeleton (P5-corvus-bringup-a)

First impl chunk of the corvus key-agent daemon. Lands a Rust `usr/corvus/` crate that runs the v1.0 hardening syscalls, verifies the kernel CSPRNG is seeded, prints a readiness banner, and exits cleanly. Production-shape orchestration: joey spawns /sbin/corvus via `SYS_SPAWN_WITH_CAPS` with `CAP_LOCK_PAGES | CAP_CSPRNG_READ`; the cap-grant flows through `rfork_with_caps`'s AND-with-parent gate; corvus inherits exactly those caps.

This chunk is **gated on `specs/corvus.tla`** (P5-corvus-spec at `c00de63`/`e5ce30a`); the skeleton's behaviour corresponds to the spec's startup-hardening discipline but doesn't yet exercise the session state machine — those verbs land at P5-corvus-bringup-b onward.

---

## Purpose

CORVUS-DESIGN.md §4.1.1 enumerates the runtime hardening corvus applies at startup:

1. `sys_mlockall(0)` — pin all currently-mapped + future-mapped pages so secrets never touch swap or any future paging tier. Sets `PROC_FLAG_MLOCKED`. Requires `CAP_LOCK_PAGES`.
2. `sys_set_dumpable(0)` — set `PROC_FLAG_NODUMP` (one-way). Future core-dump paths refuse to dump corvus.
3. `sys_set_traceable(0)` — set `PROC_FLAG_NOTRACE` (one-way). Future debug-Spoor attach paths refuse to attach to corvus.
4. `sys_getrandom(buf, 32, 0)` — verify the kernel CSPRNG is seeded (ARM RNDR via `kern_random_bytes`). Pinned by invariant C-15: corvus refuses to generate randomness until the CSPRNG is seeded.
5. `sys_explicit_bzero(buf, 32)` — wipe the probe entropy buffer. Sets the discipline that every future verb that touches a plaintext secret will follow.

After the five steps succeed, corvus prints "corvus: ready (hardening applied; CSPRNG seeded; no verbs yet)" and exits 0. The Spoor server at `/srv/corvus/` is opened by a later sub-chunk (P5-corvus-bringup-b: wire codec + verb dispatch).

The skeleton is **end-to-end testable** in the production boot path: joey's `t_spawn_with_caps + t_wait_pid` cycle reaps corvus and surfaces non-zero exits. The boot test framework's `Thylacine boot OK` marker is gated on joey's clean exit, which is gated on corvus's clean exit.

---

## Invariant correspondence (CORVUS-DESIGN.md §9)

| Invariant | Skeleton's contribution |
|---|---|
| C-2 (corvus pages are mlock'd + DONTDUMP'd) | `mlockall(0)` + `set_dumpable(0)` at startup. |
| C-5 (in-RAM secrets wiped before close returns; **RAM-wipe**, not cryptographic forward secrecy) | `explicit_bzero` pattern exercised on the probe scratch; the discipline carries forward to every future secret-handling verb. |
| C-15 (corvus refuses to generate randomness until CSPRNG verified seeded) | `getrandom(probe, 32, 0)` is the verify-on-startup probe; non-zero exit on -1 (CSPRNG unavailable). |

C-3, C-4, C-7, C-11 require the session state machine + verb dispatch + Spoor server; pinned by `specs/corvus.tla` and the impl lands at P5-corvus-bringup-b onward.

---

## ABI

`/sbin/corvus` is invoked by `joey` via `SYS_SPAWN_WITH_CAPS` with `CAP_LOCK_PAGES | CAP_CSPRNG_READ`. No args (argv is empty at v1.0; `usr/scripts/aarch64-userspace.ld` provides the entry stack via `_start`'s libthyla-rs path).

Exit status:
- `0` — hardening applied + CSPRNG seeded + probe wiped. Future sub-chunks extend this to: `0` only after a clean shutdown of the Spoor server (long-lived daemon).
- `1` — hardening failed. The stderr-equivalent diagnostic `corvus: STEP=<n> FAIL rc=<low-byte-hex>` indicates which step + the kernel-returned rc. `n=1..5` maps to mlockall / set_dumpable / set_traceable / getrandom / explicit_bzero in that order.

Boot log on success:
```
joey: spawned /sbin/corvus pid=<N>
corvus: skeleton starting (P5-corvus-bringup-a)
corvus: ready (hardening applied; CSPRNG seeded; no verbs yet)
joey: /sbin/corvus reaped status=0; hardening verified
```

---

## File layout

```
usr/corvus/
├── Cargo.toml         depends on libthyla-rs (sibling under usr/lib/)
└── src/main.rs        rs_main: 5-step hardening + banner + exit
```

`usr/lib/libthyla-rs/src/lib.rs` gained 5 new SVC wrappers + 3 cap constants:
- `T_SYS_MLOCKALL = 16`, `T_SYS_SET_DUMPABLE = 17`, `T_SYS_SET_TRACEABLE = 18`, `T_SYS_EXPLICIT_BZERO = 19`, `T_SYS_GETRANDOM = 20`
- `t_mlockall`, `t_set_dumpable`, `t_set_traceable`, `t_explicit_bzero`, `t_getrandom`
- `T_CAP_HW_CREATE`, `T_CAP_LOCK_PAGES`, `T_CAP_CSPRNG_READ`

The mirror C-side stubs already existed at P5-corvus-syscalls (`0db0dcf`/`d10d4ee`); this chunk's Rust additions complete the surface for `usr/corvus/`.

`usr/lib/libt/include/thyla/syscall.h` gained matching `T_CAP_*` macros (`(1UL << 0)`, `(1UL << 1)`, `(1UL << 2)`) so the C-side `joey.c` can express `T_CAP_LOCK_PAGES | T_CAP_CSPRNG_READ` symbolically.

---

## Orchestration

`usr/joey/joey.c` extended to chain `/hello` → `/sbin/corvus`. After verifying `/hello` exits 0, joey calls:

```c
t_spawn_with_caps("corvus", 6, T_CAP_LOCK_PAGES | T_CAP_CSPRNG_READ);
```

The kernel-side composition:

1. Plain `rfork_with_caps(CAP_ALL)` at kernel boot grants joey the full v1.0 cap ceiling (see "Joey caps bump" below).
2. `t_spawn_with_caps`'s `cap_mask` is AND'd with joey's caps (`CAP_ALL & (CAP_LOCK_PAGES | CAP_CSPRNG_READ) = CAP_LOCK_PAGES | CAP_CSPRNG_READ`). The child Proc starts with exactly those caps.
3. The child's `_start` (libthyla-rs) calls `rs_main` (corvus's main); each hardening syscall is gated on its respective cap, all of which are present.

### Joey caps bump

Pre-P5-corvus-bringup-a, `kernel/joey.c` called `rfork(RFPROC, joey_thunk, args)` which translates to `rfork_internal(... CAP_NONE)`. That gave joey zero caps. Sufficient when joey only spawned /hello (which needs no caps) but not for any cap-delegating spawn.

Change at this chunk: `rfork_with_caps(RFPROC, joey_thunk, args, CAP_ALL)`. Joey now holds the v1.0 ceiling; it's the trusted delegate that distributes caps per child's role. ARCH §28 I-2 (caps monotonically reduce) is preserved structurally: `rfork_with_caps`'s AND can only reduce.

This matches the production-init pattern. When more daemons land (P5-stratumd-bringup, P5-login, etc.), each child receives its specific cap subset; joey's CAP_ALL is the policy-decision point.

---

## Spec ↔ code mapping

Spec actions / invariants pinned at `specs/corvus.tla` (P5-corvus-spec):

| Spec action | Skeleton's correspondence |
|---|---|
| (startup hardening; out of state-machine scope) | `usr/corvus/src/main.rs::rs_main` steps 1-5 |
| `AuthSuccess` | NOT YET (deferred to P5-corvus-bringup-b's wire codec + verb dispatch) |
| `SessionTransfer`, `Unwrap`, etc. | NOT YET (verb impl) |

| Spec invariant | Skeleton's contribution |
|---|---|
| `SessionUserImmutable` (C-3) | NOT YET (no sessions in skeleton) |
| `UnwrapOwnerOnly` (C-7) | NOT YET |
| `AdminRequiresProcCap` (C-11) | NOT YET |
| `HostownerRequiresConsole` (§5.5) | NOT YET |

The skeleton's contribution is the **runtime hardening posture** that future sub-chunks inherit. Without C-2 (mlock + DONTDUMP) the secret-handling impl would leak via swap / coredump regardless of verb-level correctness; the skeleton lands that floor.

---

## Tests

The skeleton's test is the **boot path itself**. `tools/test.sh` watches for:
- `joey: spawned /sbin/corvus pid=<N>`
- `corvus: ready (hardening applied; CSPRNG seeded; no verbs yet)`
- `joey: /sbin/corvus reaped status=0; hardening verified`
- `Thylacine boot OK`

A hardening regression at any step prints `corvus: STEP=<n> FAIL rc=<rc>` and exits non-zero; joey reports `joey: /sbin/corvus exited non-zero` and itself returns 1, breaking the boot test.

The kernel-internal `test_sys_corvus.c` test suite (P5-corvus-syscalls) already covers each syscall in isolation (CAP-gate accept/reject + flag-set + idempotency + bound checks). The skeleton's role is to compose them end-to-end on a real EL0 Proc with the production cap-grant chain.

No dedicated kernel-internal test for the skeleton — the boot orchestration is the regression. Future sub-chunks (P5-corvus-bringup-b onward) add wire-level regression tests as the verb surface grows.

---

## Status

Implemented. The boot path runs corvus from a fresh kernel image; clean exit observed; default + UBSan suites both pass 442/442.

Out of scope for this chunk (deferred to P5-corvus-bringup-b onward):
- `/srv/corvus/` Spoor server bring-up.
- Binary frame wire codec.
- AUTH / UNWRAP / SESSION_CLOSE / ADMIN_ELEVATE verb dispatch.
- Crypto primitives (Argon2id + AEGIS-256-AEAD + ML-KEM-768 + X25519).
- State file format (magic `CRVS`).
- Encrypted audit log.
- Rate limiter.

---

## Known caveats

- The skeleton exits 0 immediately after hardening. Production corvus is long-lived; the skeleton's "exit 0" is a TEST signal, not a daemon discipline. P5-corvus-bringup-b replaces the exit with an infinite Spoor-server loop.
- `t_set_dumpable(0)` and `t_set_traceable(0)` are forward-compat: at v1.0 there are no core-dump or debug-Spoor subsystems to gate. The syscalls set the flags so the gate is in place when those subsystems land.
- `t_explicit_bzero` on the 32-byte probe is exercise-only at this chunk — there are no real secrets in the skeleton. The codepath validation is the point.
- The probe entropy (32 bytes from `kern_random_bytes`) is not statistically checked; the skeleton just verifies the syscall returns `PROBE_LEN`. A buggy CSPRNG returning correlated bytes would not be caught here — a future P5-tsan-enable or fuzz-harness chunk could add entropy sanity checks.

---

## References

- `docs/CORVUS-DESIGN.md §4.1.1` — startup hardening discipline.
- `docs/CORVUS-DESIGN.md §9` — invariants (C-2, C-5, C-15 pinned at this chunk's runtime contribution; C-3 / C-7 / C-11 deferred).
- `docs/CORVUS-DESIGN.md §10` — implementation arc (P5-corvus-bringup-a is sub-chunk a of P5-corvus-bringup).
- `specs/corvus.tla` — gating spec for verb dispatch (P5-corvus-spec).
- `specs/SPEC-TO-CODE.md` — corvus.tla section's action↔impl-target table; impl targets are populated as sub-chunks land.
- `docs/reference/58-corvus-syscalls.md` — the syscall surface this chunk's skeleton consumes.
- `docs/reference/63-sys-spawn-with-caps.md` — the cap-delegating spawn mechanism.
