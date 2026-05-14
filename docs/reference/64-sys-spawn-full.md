# 64. SYS_SPAWN_FULL — combined fds + caps spawning (P5-spawn-full)

The fourth and most general spawn variant. Unions `SYS_SPAWN_WITH_FDS` (fd inheritance, KOBJ_SPOOR-only) with `SYS_SPAWN_WITH_CAPS` (cap-subset via `rfork_with_caps`). Needed at `P5-corvus-bringup` where joey spawns `/sbin/corvus` with a pipe pair (for login communication) AND `CAP_LOCK_PAGES | CAP_CSPRNG_READ`.

The four spawn variants now in v1.0:

| # | Syscall | Fds | Caps | Use case |
|---|---|---|---|---|
| 21 | `SYS_SPAWN` | no | zero | `/joey` spawning `/hello` for orchestration tests. |
| 23 | `SYS_SPAWN_WITH_FDS` | yes | zero | `/stub-driver` spawning `/stratumd-stub` over pipes. |
| 24 | `SYS_SPAWN_WITH_CAPS` | no | subset | future: spawn a privileged daemon that opens its own fds. |
| 25 | `SYS_SPAWN_FULL` | yes | subset | `/joey` spawning `/sbin/corvus`, `/sbin/login`, `/sbin/stratumd`. |

The four coexist because each call site reads more clearly with a variant that matches its needs. Future consolidation (if it ever happens) is internal — the syscall ABIs are stable.

---

## ABI

```
SYS_SPAWN_FULL(name_va, name_len, fd_list_va, fd_count, cap_mask) → child_pid (>0) / -1

Args:
  x0 = name_va        user-VA pointer to the binary name
  x1 = name_len       bytes; 1..SYS_SPAWN_NAME_MAX = 64
  x2 = fd_list_va     user-VA pointer to u32[fd_count] (or 0 if fd_count==0)
  x3 = fd_count       0..SYS_SPAWN_MAX_FDS = 16
  x4 = cap_mask       caps_t bitmask; child gets parent->caps & mask

Return:
  x0 = child_pid      >0 on success
  x0 = -1             on the union of SYS_SPAWN_WITH_FDS and
                      SYS_SPAWN_WITH_CAPS failure conditions
```

---

## Implementation

`sys_spawn_full_for_proc` is structurally identical to `sys_spawn_with_fds_for_proc` with one substitution: `rfork_with_caps(RFPROC, sys_spawn_with_fds_thunk, sa, cap_mask)` instead of `rfork(RFPROC, sys_spawn_with_fds_thunk, sa)`. The thunk (`sys_spawn_with_fds_thunk`) is shared — it doesn't know or care about caps; cap arithmetic happens inside `rfork_with_caps` BEFORE the thunk runs.

Refcount discipline (per-fd `spoor_ref` bump → child consumes via `handle_alloc`; failure paths drop bumps symmetrically) is unchanged from `SYS_SPAWN_WITH_FDS`. Cap arithmetic (structural enforcement of ARCH I-2 / I-6 via `parent->caps & cap_mask`) is unchanged from `SYS_SPAWN_WITH_CAPS`.

---

## Userspace API — `<thyla/syscall.h>`

```c
long t_spawn_full(const char *name, size_t name_len,
                  const unsigned int *fds, size_t fd_count,
                  unsigned long cap_mask);
```

Five-argument inline-asm stub using x0..x4 + x8 for the syscall number.

---

## Tests

`kernel/test/test_sys_spawn_full.c` — 5 tests:

- `sys_spawn_full.happy_path_fds_and_caps` — pipe `rd` end passed to child + `cap_mask=CAP_LOCK_PAGES`; verify `child->caps == CAP_LOCK_PAGES` (race-free since caps are set inside `rfork_with_caps` before the thread is ready'd) AND `/hello` exits cleanly (indirect proof that fd installation + exec_setup succeeded).
- `sys_spawn_full.zero_count_zero_mask_succeeds` — `fd_count=0` + `cap_mask=CAP_NONE` → equivalent to `SYS_SPAWN`; child has no fds, no caps, exits cleanly.
- `sys_spawn_full.rejects_oversize_fd_count` — `fd_count > SYS_SPAWN_MAX_FDS` → -1.
- `sys_spawn_full.rejects_bad_fd` — `fd_list` contains `UINT32_MAX` → -1.
- `sys_spawn_full.rejects_missing_binary` — name = `nonexistent` → -1.

The cap arithmetic IS inspected directly via `proc_find_by_pid` (caps are stable for the Proc's lifetime; safe to read before `wait_pid`). The fd installation is NOT inspected directly — the thunk runs asynchronously on the child's CPU, and inspecting `child->handles[]` from the test would race with `handle_alloc`. The fd path is structurally identical to `SYS_SPAWN_WITH_FDS` (same thunk + args struct), already covered end-to-end by `userspace.stub_driver_round_trip`. The clean-exit assertion in the happy_path test is the indirect check that fd installation succeeded.

Test count: 435 → 440 PASS × default + UBSan.

---

## Mid-chunk discovery: thunk-side handle_alloc races with parent inspection

First draft of `happy_path_fds_and_caps` checked `handle_get(child, 0)` directly after `sys_spawn_full_for_proc` returned. That failed with `child has handle at fd 0` because the thunk's `handle_alloc` ran on the child's CPU asynchronously; the parent's inspection beat it.

Resolution: do not inspect the child's handle table from the parent test. Verify fd inheritance indirectly via the clean-exit assertion + rely on the structural identity with `SYS_SPAWN_WITH_FDS` (whose end-to-end test, `stub_driver_round_trip`, exercises the full inheritance path).

The race window is bounded — the thunk runs to completion (handle_alloc + exec_setup + userland_enter) before the child's `main` returns. But for kernel-internal inspection from a different thread, the only race-free approach is wait_pid (which then frees the child's handles before we can read them).

A future test could poll `child->handles[0] != NULL` with `sched()` yields between checks. Not worth the complexity at this chunk; the existing coverage is sufficient.

---

## Composition with future chunks

- **P5-corvus-bringup**: joey will call `t_spawn_full("corvus", 6, [corvus_login_pipe_end], 1, T_CAP_LOCK_PAGES | T_CAP_CSPRNG_READ)`. Corvus inherits the pipe end at fd 0 (its handle to login communication) AND the caps it needs for `t_mlockall` + `t_getrandom`.
- **P5-login**: joey will spawn `/sbin/login` similarly with the other end of the corvus-login pipe + console fd + `T_CAP_HOSTOWNER` (when that cap lands at P5-hostowner).

---

## Status

| Item | State |
|---|---|
| `SYS_SPAWN_FULL` handler + `sys_spawn_full_for_proc` inner | LANDED |
| libt `t_spawn_full` stub | LANDED |
| 5 kernel-internal tests (1 happy path + 1 zero-zero happy + 3 rejection) | LANDED |
| Userspace production probe exercising SYS_SPAWN_FULL with non-zero caps | DEFERRED (lands as part of P5-corvus-bringup) |

---

## Known caveats

1. **Four spawn variants now.** SYS_SPAWN_FULL subsumes the others (caller can pass `fd_count=0` and/or `cap_mask=CAP_NONE`). The earlier variants stay because each call site reads more clearly with a variant that matches its needs.
2. **No userspace probe with non-zero caps yet.** /stub-driver passes `fd_count=2` to stratumd-stub but `cap_mask=0` — the stub doesn't need any caps. A userspace test exercising `SYS_SPAWN_FULL` with non-zero caps lands when a cap-needing binary becomes available (corvus or a synthetic test binary).
3. **Inherits the SYS_SPAWN_WITH_FDS race**: the thunk-side handle_alloc isn't inspectable from the parent without a race. Documented above; resolved by relying on end-to-end coverage.
