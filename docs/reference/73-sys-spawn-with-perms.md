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

`SPAWN_PERM_ALL` is the OR of all valid bits — used by the kernel + userspace wrappers for the validity check.

Adding a new `SPAWN_PERM_*` bit is additive: existing callers (passing 0 or only the documented bits) behave identically. Each new bit must:

1. Define a kernel-side `proc_mark_*` function (one-way, idempotent, fail-closed on a destroyed Proc).
2. Add a case to the spawn thunk's bit dispatch.
3. Extend `SPAWN_PERM_ALL`.
4. Mirror the bit in libt + libthyla-rs.
5. Pin the gate: any new bit gating user-visible authority should require the calling Proc to be console-attached (same rule as the v1.0 bit), unless the design explicitly justifies a different gate.

---

## Console-attachment gate

Any `perm_flags != 0` is rejected unless the calling Proc holds `PROC_FLAG_CONSOLE_ATTACHED`. The check fires in `sys_spawn_with_perms_for_proc` (kernel/syscall.c) BEFORE any user-VA reads, so a hostile caller cannot probe the gate behavior with arbitrary side effects.

At v1.0 the only console-attached Proc is joey (stamped in `joey_thunk`, kernel/joey.c). joey is the trust anchor: ARCHITECTURE §5.5 calls it "the local-console root of the trust chain." Any future Proc that wants to confer `SPAWN_PERM_*` bits on its children must first become console-attached (`proc_mark_console_attached` is kernel-only at v1.0; the design defers a user-visible "I have the console" syscall to v1.x).

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

Both wrappers also expose the bit-mask constants (`T_SPAWN_PERM_MAY_POST_SERVICE`).

---

## Tests

`kernel/test/test_sys_spawn_with_perms.c` — 4 tests:

- `sys_spawn_with_perms.zero_perm_is_spawn_full` — `perm_flags=0` from kproc → positive pid, child has no may-post stamp, exits clean.
- `sys_spawn_with_perms.console_attached_grants_may_post` — marks kproc console-attached, spawns with `SPAWN_PERM_MAY_POST_SERVICE`; verifies child spawns clean and does NOT inherit `PROC_FLAG_CONSOLE_ATTACHED` (rfork strips it; the spawn thunk does NOT re-stamp it).
- `sys_spawn_with_perms.rejects_non_console_attached_parent` — fresh non-console-attached Proc as parent → -1.
- `sys_spawn_with_perms.rejects_unknown_perm_bits` — even a console-attached parent passing garbage high bits → -1.

Test count: 507 → 511 PASS × default + UBSan.

The console_attached_grants_may_post test marks kproc console-attached as a one-way side effect. Post-test boot path is unaffected: rfork does not propagate the console bit, so joey starts un-attached and self-stamps in `joey_thunk` (the existing pattern). kproc carrying the bit after the test is benign.

---

## Status

| Aspect | Status |
|---|---|
| Kernel implementation | LANDED at P5-corvus-srv-impl-b3a *(pending)* |
| libt wrapper | LANDED |
| libthyla-rs wrapper | LANDED |
| Tests | 4 PASS × default + UBSan |
| Audit | self-audit clean; formal audit at P5-corvus-srv-impl audit (#525) |
| Production caller | joey at P5-corvus-srv-impl-b3b (#543) |

---

## Known caveats / footguns

- **The thunk's bit dispatch is open-coded**, not a generic loop. Adding a new `SPAWN_PERM_*` bit means modifying the thunk's `if`-ladder. The defense is the `if (perm_flags & ~SPAWN_PERM_ALL) extinction(...)` guard at the tail of the dispatch: an unknown bit at thunk time is a kernel invariant violation (the parent path must have rejected it).
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
