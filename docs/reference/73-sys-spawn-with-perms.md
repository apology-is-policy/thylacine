# 73. SYS_SPAWN_WITH_PERMS — spawn + atomic PROC_FLAG_* stamp (P5-corvus-srv-impl-b3a)

The fifth spawn variant. Extends `SYS_SPAWN_FULL` with a sixth argument `perm_flags` carrying `SPAWN_PERM_*` bits that the kernel stamps on the spawned child Proc atomically inside the spawn thunk — BEFORE the child's `exec_setup`, so the child's very first user-mode instruction observes the final `proc_flags`.

The race this closes: the design ([CORVUS-DESIGN.md §6.1](../CORVUS-DESIGN.md)) requires joey to confer `PROC_FLAG_MAY_POST_SERVICE` on `/sbin/corvus` so corvus may call `SYS_POST_SERVICE("corvus")`. The naive "mark after spawn" pattern (`spawn → mark(child_pid)`) leaves a window in which the child could reach `SYS_POST_SERVICE` before the parent's mark lands — particularly on SMP where the child can run on another CPU between the parent's spawn-return and the parent's next syscall. Baking the stamp into the spawn thunk eliminates the window entirely: the thunk runs in the child's thread context, the stamp is applied before any user code, and the child's first instruction reads the final flags.

The five spawn variants now in v1.0:

| # | Syscall | Fds | Caps | Perms | Use case |
|---|---|---|---|---|---|
| 21 | `SYS_SPAWN` | no | zero | no | `/joey` spawning `/hello` for orchestration tests. |
| 23 | `SYS_SPAWN_WITH_FDS` | yes | zero | no | `/stub-driver` spawning `/stratumd-stub` over pipes. |
| 24 | `SYS_SPAWN_WITH_CAPS` | no | subset | no | future: spawn a privileged daemon that opens its own fds. |
| 25 | `SYS_SPAWN_FULL` | yes | subset | no | `/joey` spawning `/sbin/corvus` (b1/b2) with pipe + cap subset. |
| 31 | `SYS_SPAWN_WITH_PERMS` | yes | subset | yes | `/joey` spawning `/sbin/corvus` (b3b+) with may-post grant. |

---

## ABI

```
SYS_SPAWN_WITH_PERMS(name_va, name_len, fd_list_va, fd_count, cap_mask, perm_flags)
                                                                → child_pid (>0) / -1

Args:
  x0 = name_va        user-VA pointer to the binary name
  x1 = name_len       bytes; 1..SYS_SPAWN_NAME_MAX = 64
  x2 = fd_list_va     user-VA pointer to u32[fd_count] (or 0 if fd_count==0)
  x3 = fd_count       0..SYS_SPAWN_MAX_FDS = 16
  x4 = cap_mask       caps_t bitmask; child gets parent->caps & mask
  x5 = perm_flags     SPAWN_PERM_* bitmask; kernel stamps each set bit
                      onto the child's proc_flags inside the spawn thunk

Return:
  x0 = child_pid      >0 on success
  x0 = -1             on the union of SYS_SPAWN_FULL failure conditions
                      PLUS perm_flags-specific:
                        - perm_flags has bits outside SPAWN_PERM_ALL
                        - any perm_flags bit set AND caller not console-attached
```

---

## SPAWN_PERM_* bits

Pinned in `<thylacine/syscall.h>`; mirror constants in `<thyla/syscall.h>` (C-side) and `libthyla_rs::T_SPAWN_PERM_*` (Rust-side).

| Bit | Constant | Effect |
|---|---|---|
| 1<<0 | `SPAWN_PERM_MAY_POST_SERVICE` | Stamps `PROC_FLAG_MAY_POST_SERVICE` on the child. The child may then call `SYS_POST_SERVICE` to register as a `/srv/<name>` 9P server (CORVUS-DESIGN.md §6.1). |
| 1<<1 | `SPAWN_PERM_CONSOLE_TRUSTED` | (A-4c-2) Records the child as the trusted login authority (`g_console_trusted_proc`) — the Proc a kernel SAK re-grants the console to. joey grants it to `/sbin/corvus`. The SAK/elevation anchor; console-attach-only (never delegable). |
| 1<<2 | `SPAWN_PERM_CONSOLE_OWNER` | (LS-5) Records the child as the console OWNER (`g_console_owner`) — the Proc that receives the `interrupt` note on Ctrl-C. Console-*owner* ("who receives Ctrl-C"), strictly distinct from console-*attach* (the owner bit never confers attach, so I-27 is untouched). joey-trusted `/sbin/login` confers it on the session shell `ut`. |

`SPAWN_PERM_ALL` is the OR of all valid bits — used by the kernel + userspace wrappers for the validity check.

Adding a new `SPAWN_PERM_*` bit is additive: existing callers (passing 0 or only the documented bits) behave identically. Each new bit must:

1. Define a kernel-side `proc_mark_*` function (one-way, idempotent, fail-closed on a destroyed Proc).
2. Add a case to the spawn thunk's bit dispatch.
3. Extend `SPAWN_PERM_ALL`.
4. Mirror the bit in libt + libthyla-rs.
5. Pin the gate in `spawn_perm_grant_check`: choose console-attach-only (the SAK-anchor rule, like `CONSOLE_TRUSTED`) or the holder-delegable rule (console-attached OR holds `MAY_POST_SERVICE`, like `MAY_POST_SERVICE` / `CONSOLE_OWNER`), per the design's trust model.

---

## The grant gate (`spawn_perm_grant_check`)

The grant authority is per-bit, in `spawn_perm_grant_check` (kernel/syscall.c) — the single site both spawn entry points (`SYS_SPAWN_WITH_PERMS` and `SYS_SPAWN_FULL_ARGV`) route through. It runs BEFORE any user-VA reads, so a hostile caller cannot probe gate behavior with side effects, and any bit outside `SPAWN_PERM_ALL` is rejected outright.

Per-bit rules:

- **`CONSOLE_TRUSTED`** — console-attach-only (`proc_is_console_attached`). NEVER delegable: a service-poster must not be able to confer the console-trust used for hostowner elevation (I-27).
- **`MAY_POST_SERVICE`** and **`CONSOLE_OWNER`** — granted by a console-attached Proc OR by a Proc that already holds `MAY_POST_SERVICE` (the A-5b #827b one-hop delegation). joey (console-attached, stamped in `joey_thunk`) is the trust root; it confers `MAY_POST_SERVICE` on `/sbin/login`, which — now a holder — re-confers `MAY_POST_SERVICE` on the per-user proxy and (LS-5) `CONSOLE_OWNER` on the session shell `ut`.

joey is the trust anchor: ARCHITECTURE §5.5 calls it "the local-console root of the trust chain." `proc_mark_console_attached` is kernel-only at v1.0; a user-visible "I have the console" syscall is deferred to v1.x. None of these bits is `rfork`-propagated — each is a `perm_flags` spawn-time decision, not a `cap_mask` bit, so I-2 (the fork-grantable cap set only reduces) is unaffected.

The thunk applies the granted bits via `apply_spawn_perms` (kernel/syscall.c) — the shared bit→`proc_mark_*`/`proc_set_*` mapping both spawn thunks call in the child's context, before `exec_setup`, so the child's first user-mode instruction observes the final state.

A `perm_flags = 0` call is equivalent to `SYS_SPAWN_FULL` and does NOT require console-attachment — kept as a single entry point so callers conferring no permissions don't need a separate syscall.

---

## Implementation

`kernel/syscall.c::sys_spawn_with_perms_for_proc` is the public entry point; it gate-checks `perm_flags` and dispatches to the shared core `sys_spawn_full_with_perms_for_proc`. The shared core does the full-fledged spawn (fd validation + bump, ELF blob lookup + copy, args-struct alloc, `rfork_with_caps`); the `perm_flags` are passed through to the args struct (`struct spawn_with_fds_args::perm_flags`).

The spawn thunk (`sys_spawn_with_fds_thunk`) reads `sa->perm_flags` into a local **before** `kfree(sa)`, then — once `current_thread()->proc` is the child — applies each set bit BEFORE the fd install loop and the `exec_setup`. The order is load-bearing: the stamp must precede any code path the child could execute. The fd install + exec_setup are kernel-only too, but those don't transition to user mode; the proof that the stamp wins is structural (no `userland_enter` runs before the stamp).

`sys_spawn_full_for_proc` is now a thin wrapper that calls `sys_spawn_full_with_perms_for_proc` with `perm_flags=0`. External callers (the SYS_SPAWN_FULL handler + test_sys_spawn_full.c) see no signature change.

---

## Userspace API

**`<thyla/syscall.h>`** (C):

```c
long t_spawn_with_perms(const char *name, size_t name_len,
                        const unsigned int *fds, size_t fd_count,
                        unsigned long cap_mask,
                        unsigned long perm_flags);
```

Six-argument inline-asm stub using x0..x5 + x8 for the syscall number.

**`libthyla_rs::t_spawn_with_perms`** (Rust):

```rust
pub unsafe fn t_spawn_with_perms(name: *const u8, name_len: usize,
                                 fds: *const u32, fd_count: usize,
                                 cap_mask: u64, perm_flags: u64) -> i64;
```

Both wrappers also expose the bit-mask constants (`T_SPAWN_PERM_MAY_POST_SERVICE` / `T_SPAWN_PERM_CONSOLE_TRUSTED` / `T_SPAWN_PERM_CONSOLE_OWNER`).

---

## Tests

`kernel/test/test_sys_spawn_with_perms.c` — 8 tests:

- `sys_spawn_with_perms.zero_perm_is_spawn_full` — `perm_flags=0` from kproc → positive pid, child has no may-post stamp, exits clean.
- `sys_spawn_with_perms.console_attached_grants_may_post` — marks kproc console-attached, spawns with `SPAWN_PERM_MAY_POST_SERVICE`; verifies child spawns clean and does NOT inherit `PROC_FLAG_CONSOLE_ATTACHED` (rfork strips it; the spawn thunk does NOT re-stamp it).
- `sys_spawn_with_perms.rejects_non_console_attached_parent` — fresh non-console-attached Proc as parent → -1.
- `sys_spawn_with_perms.rejects_unknown_perm_bits` — even a console-attached parent passing garbage high bits → -1.
- `sys_spawn_with_perms.holder_delegates_may_post` (A-5b #827b) — a Proc that already holds `MAY_POST_SERVICE` but is NOT console-attached delegates it one hop; the control (non-attached non-holder) is still rejected.
- `sys_spawn_with_perms.console_trusted_not_delegable` (A-5b #827b) — a `MAY_POST_SERVICE` holder cannot confer `CONSOLE_TRUSTED` (alone or combined); a console-attached Proc grants both.
- `sys_spawn_with_perms.console_owner_grant_gate` (LS-5) — `CONSOLE_OWNER` is granted by a `MAY_POST_SERVICE` holder (trusted login) OR a console-attached Proc, rejected from a non-attached non-holder, and never unlocks `CONSOLE_TRUSTED` for a mere holder (I-27).
- `sys_spawn_with_perms.console_owner_set_wiring` (LS-5) — drives `apply_spawn_perms` directly: `CONSOLE_OWNER` makes the child `g_console_owner` (and does NOT confer console-attach); `MAY_POST_SERVICE` alone leaves the owner untouched. (A real spawn races the child's exit clearing the owner, so the wiring is driven on a synthetic Proc.)

All run × default + UBSan.

The console_attached_grants_may_post test marks kproc console-attached as a one-way side effect. Post-test boot path is unaffected: rfork does not propagate the console bit, so joey starts un-attached and self-stamps in `joey_thunk` (the existing pattern). kproc carrying the bit after the test is benign.

---

## Status

| Aspect | Status |
|---|---|
| Kernel implementation | LANDED at P5-corvus-srv-impl-b3a; extended `CONSOLE_TRUSTED` (A-4c-2), one-hop delegation (A-5b #827b), `CONSOLE_OWNER` (LS-5a) |
| libt wrapper | LANDED (`MAY_POST_SERVICE` / `CONSOLE_TRUSTED` / `CONSOLE_OWNER`) |
| libthyla-rs wrapper | LANDED (+ `Command::perm`) |
| Tests | 8 PASS × default + UBSan |
| Audit | formal audit at P5-corvus-srv-impl (#525); `CONSOLE_OWNER` rides the LS-5-audit round (#963) |
| Production callers | joey → corvus (`MAY_POST_SERVICE` + `CONSOLE_TRUSTED`); joey → /sbin/login (`MAY_POST_SERVICE`); /sbin/login → ut (`CONSOLE_OWNER`) + per-user proxy (`MAY_POST_SERVICE`) |

---

## Known caveats / footguns

- **The bit dispatch is open-coded** in `apply_spawn_perms` (shared by both spawn thunks), not a generic loop. Adding a new `SPAWN_PERM_*` bit means modifying that `if`-ladder. The defense is the `if (perm_flags & ~SPAWN_PERM_ALL) extinction(...)` guard at its tail: an unknown bit at thunk time is a kernel invariant violation (the parent's `spawn_perm_grant_check` must have rejected it).
- **Console-attachment is a permanent one-way set.** A Proc that becomes console-attached cannot un-set the bit. v1.0 has only joey as the console anchor; future remote-login / multi-console designs need to revisit this.
- **The kproc-stamp side effect in tests is intentional.** Tests deliberately stamp kproc console-attached to exercise the happy path. Subsequent tests must not assume kproc is NOT console-attached.

---

## References

- `kernel/include/thylacine/syscall.h` — SYS_SPAWN_WITH_PERMS + SPAWN_PERM_* + SPAWN_PERM_ALL.
- `kernel/syscall.c::sys_spawn_with_perms_for_proc` + `sys_spawn_with_perms_handler` + `sys_spawn_with_fds_thunk` (thunk-side stamp).
- `usr/lib/libt/include/thyla/syscall.h::t_spawn_with_perms`.
- `usr/lib/libthyla-rs/src/lib.rs::t_spawn_with_perms`.
- `kernel/test/test_sys_spawn_with_perms.c`.
- [CORVUS-DESIGN.md §6.1](../CORVUS-DESIGN.md) — the design rationale.
- [ARCHITECTURE.md §11.2c](../ARCHITECTURE.md) — the v1.0 syscall surface.
- [reference/14-process-model.md](14-process-model.md) — `PROC_FLAG_MAY_POST_SERVICE` semantics.
