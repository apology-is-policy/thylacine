# 62. SYS_SPAWN_WITH_FDS — fd inheritance on spawn (P5-stratumd-stub-bringup-b)

Extends `SYS_SPAWN` with explicit fd inheritance: the caller names a list of fds (which must all be `KOBJ_SPOOR` in its handle table at v1.0) and those fds are installed in the spawned child's handle table at slots `0..fd_count-1` **before** `exec_setup` runs. The new userspace binary `/stub-driver` exercises the full production-shape orchestration: it spawns `/stratumd-stub` with two pipe ends pre-installed, drives the 9P attach + mount + unmount cycle, and reaps the stub — all from EL0, no kernel-supervised setup.

This is sub-chunk **b** in the P5-stratumd-stub-bringup arc. With a + b landed, joey-style orchestration is feasible entirely from userspace. Sub-chunks c (long-running stub serving multiple clients), d (pivot/chroot mechanism), and e (full joey-in-production) remain.

---

## Why a separate syscall instead of extending SYS_SPAWN

`SYS_SPAWN(name_va, name_len)` reads only x0 + x1 from the SVC context. Adding more args to it would require either (a) ABI-breaking the libt stub (existing callers' x2/x3 hold garbage that would suddenly become meaningful) or (b) all-callers-pass-zero discipline. Neither is appealing. A separate `SYS_SPAWN_WITH_FDS` keeps `SYS_SPAWN`'s shape stable and makes the inheritance intent explicit at the call site.

Plan 9's `rfork(RFFDG)` is the more general primitive (copy parent's entire fd table). At v1.0 that's deferred — the explicit-list shape covers the Thylacine use case (joey passes 2-3 specific Spoors to a child) more cleanly than "copy everything then close most." `SYS_RFORK` with `RFFDG` lands when a v1.x workload (shell pipelines, musl `clone()` translation) needs the general primitive.

---

## ABI

```
SYS_SPAWN_WITH_FDS(name_va, name_len, fd_list_va, fd_count) → child_pid (>0) / -1

Args:
  x0 = name_va        user-VA pointer to the binary name
  x1 = name_len       bytes; 1..SYS_SPAWN_NAME_MAX = 64
  x2 = fd_list_va     user-VA pointer to u32[fd_count] (or 0 if fd_count==0)
  x3 = fd_count       0..SYS_SPAWN_MAX_FDS = 16

Return:
  x0 = child_pid      >0 on success
  x0 = -1             on:
       - same as SYS_SPAWN (name validation / binary lookup / blob size / OOM)
       - fd_count > SYS_SPAWN_MAX_FDS
       - fd_list_va bound violation (when fd_count > 0)
       - any fd in the list is not a valid open handle in the caller
       - any fd in the list is not KOBJ_SPOOR
```

Child rights on each inherited fd: `RIGHT_READ | RIGHT_WRITE | RIGHT_TRANSFER` (mirrors `SYS_PIPE`).

The parent retains its own holds. This is **"give the child its own ref,"** not **"transfer."** A child that closes its inherited fds doesn't affect the parent's holds; a parent that closes its copies doesn't affect the child's.

---

## Refcount discipline

For each fd in the inheritance list, the handler takes an **additional** `spoor_ref` on the underlying Spoor. Those bumped refs are owned by the kmalloc'd `spawn_with_fds_args` struct until the child's thunk consumes them via `handle_alloc` (which transfers the ref into the child's handle-table slot).

Failure paths:
- **Validation failure on any fd** (bad index / wrong kind): the handler drops all refs bumped so far, returns -1.
- **`devramfs_lookup` miss / blob too large / kmalloc OOM**: drops all bumped refs.
- **`rfork` failure**: drops all bumped refs + kfrees the blob and args struct.
- **Inside the thunk, `handle_alloc` returns the wrong fd index**: thunk drops un-installed refs explicitly via `spoor_clunk`; installed prefix gets cleaned up by `proc_free`'s `handle_table_free`. Thunk `exits("fail-fd-install")` so the parent's `SYS_WAIT_PID` sees a non-zero status.
- **`exec_setup` failure**: installed handles are cleaned up by `proc_free`. Thunk `exits("fail-exec")`.

Net invariant: under any failure, refcounts balance — every bump matches an unref or a handle_alloc-consume.

---

## Why KOBJ_SPOOR-only at v1.0

The kind check restricts each inherited fd to `KOBJ_SPOOR`:

1. **Use case coverage**: pipes + 9P transports are the only handle kind that fits the "process A creates, process B serves" model at v1.0.
2. **ARCH I-5**: `KOBJ_MMIO`, `KOBJ_IRQ`, `KOBJ_DMA` are explicitly non-transferable across Procs. Hardware handles must be created in the holding Proc; inheritance breaks the model.
3. **`KOBJ_BURROW`**: deferable. A v1.x workload that needs shared memory across `rfork` (e.g., a shell with shared `tmpfs`) will add this. For now, the rejection is explicit and tested.

Future expansion: bump the syscall's accepted kinds via the same handler with no ABI change. The existing rejection test (`sys_spawn_with_fds.rejects_non_spoor_fd`) will need updating when burrow inheritance lands.

---

## Userspace API — `<thyla/syscall.h>`

```c
long t_spawn_with_fds(const char *name, size_t name_len,
                      const unsigned int *fds, size_t fd_count);

#define T_SPAWN_MAX_FDS  16u
```

Inline-asm stub matching the kernel ABI shape. `fds` is a `const unsigned int *` (each fd is a 32-bit handle-table index).

---

## /stub-driver — the production-shape orchestrator

`usr/stub-driver/stub-driver.c` (~110 LOC) is a new userspace binary that does the full joey-style orchestration:

1. `t_pipe × 2` → 4 fds: `c2s_rd`, `c2s_wr`, `s2c_rd`, `s2c_wr`.
2. `t_spawn_with_fds("stratumd-stub", 13, { c2s_rd, s2c_wr }, 2)` → spawns the stub with two fds pre-installed (its fd 0 = c2s_rd for rx; fd 1 = s2c_wr for tx).
3. `t_close(c2s_rd)` + `t_close(s2c_wr)` — driver drops its driver-side copies. Without this, the c2s/s2c rings still have readers/writers via the driver, and the stub can't see EOF when the driver finishes.
4. `t_attach_9p(c2s_wr, s2c_rd, "/", 1, 0)` → drives Tversion + Tattach against the stub.
5. `t_mount(attach_fd, 99, 0)`.
6. `t_unmount(99)`.
7. `t_close(attach_fd)` — last drop on the dev9p Spoor; tears down the attach session (Tclunk on root_fid is rejected at session layer; no wire op).
8. `t_close(c2s_wr)` + `t_close(s2c_rd)` — driver drops its last refs. `c2s_wr` last drop fires `write_eof` on the c2s ring; the stub's next read returns 0; stub exits.
9. `t_wait_pid` for the stub; expects status=0.
10. `t_putstr("stub-driver: PASS\n")` + `t_exits(0)`.

The driver is the production shape — what joey will look like at boot, only with `stratumd-system` instead of `stratumd-stub` and with additional capability/mount/pivot work between steps 9 and 10.

---

## Kernel test

`kernel/test/test_stub_driver.c` (~85 LOC) is minimal: load `/stub-driver` from devramfs, `rfork-exec` it, `wait_pid` and assert status=0. The driver itself does all the real work in userspace.

Boot log on success:

```
/stub-driver size=17280 → exec; driver orchestrates stratumd-stub itself
stratumd-stub: serving on fd 0 (rx) + fd 1 (tx)
stratumd-stub: EOF on rx; exit 0
stub-driver: PASS
/stub-driver reaped pid=N status=0 — SYS_SPAWN_WITH_FDS production-shape orchestration verified end-to-end
```

This proves that `SYS_SPAWN_WITH_FDS` works end-to-end at EL0 with no kernel-supervised setup. The `test_stratumd_stub_round_trip` (P5-stratumd-stub-bringup-a) coexists as the smaller kernel-supervised version — both stay in the test matrix because they cover different code paths.

---

## Rejection tests

`kernel/test/test_sys_spawn_with_fds.c` covers the validation surface end-to-end via `sys_spawn_with_fds_for_proc`:

- `sys_spawn_with_fds.rejects_oversize_fd_count` — `fd_count > SYS_SPAWN_MAX_FDS` → -1.
- `sys_spawn_with_fds.rejects_bad_fd` — fd outside handle-table range → -1.
- `sys_spawn_with_fds.rejects_non_spoor_fd` — Burrow handle in the list → -1.
- `sys_spawn_with_fds.rejects_missing_binary` — name = `nonexistent` → -1.
- `sys_spawn_with_fds.zero_count_succeeds` — `fd_count = 0` is equivalent to `SYS_SPAWN` (no inheritance); happy path with `hello`.

Each test calls `drain_zombies()` first for deterministic `wait_pid` coverage.

Test count: 424 → 430 PASS × default + UBSan (+1 happy-path `stub_driver_round_trip` + 5 rejection paths + 1 `zero_count_succeeds` happy path = +7).

---

## Composition with future chunks

- **P5-stratumd-stub-bringup-c**: long-running stub serving multiple sequential clients. The stub's loop already supports this; what's missing is the demonstration. Future: stub-driver spawns stub once, attaches+mounts+unmounts multiple times, then closes everything.
- **P5-stratumd-stub-bringup-d**: pivot/chroot mechanism. Once a stratumd-served tree is mounted, no current mechanism lets the Proc treat it as namespace root. `pivot_root`-equivalent or `chdir`-on-mount-handle.
- **P5-stratumd-stub-bringup-e**: full joey-in-production. Joey's main() does the stub-driver dance against real `stratumd-system` (when cross-compiled Stratum lands).
- **P5-corvus-bringup**: corvus startup uses SYS_SPAWN_WITH_FDS to inherit its `/srv/corvus/` Spoor pair from joey-or-login.

---

## Status

| Item | State |
|---|---|
| `SYS_SPAWN_WITH_FDS` handler + `sys_spawn_with_fds_for_proc` inner | LANDED |
| libt `t_spawn_with_fds` + `T_SPAWN_MAX_FDS` macro | LANDED |
| `/stub-driver` userspace binary | LANDED |
| Production-shape orchestration verified at EL0 end-to-end | LANDED |
| KOBJ_SPOOR-only kind check | LANDED |
| 5 rejection-path tests + 1 zero-count happy + 1 production-shape happy | LANDED |
| Burrow inheritance | DEFERRED (v1.x; rejection test updates when it lands) |
| MMIO/IRQ/DMA inheritance | EXPLICITLY-NEVER (ARCH I-5) |
| Long-running stub demonstration | DEFERRED (P5-stratumd-stub-bringup-c) |
| Pivot/chroot mechanism | DEFERRED (P5-stratumd-stub-bringup-d) |

---

## Known caveats

1. **No environment variable inheritance.** SYS_SPAWN_WITH_FDS passes fds but not argv/envp; the child binary takes no arguments at v1.0. Future expansion via a sibling syscall or a generalized `SYS_EXECVE`.
2. **Driver-side refcount discipline is load-bearing.** If the driver forgets to `t_close` its driver-side copies of fds it passed to the child, the stub can't see EOF (the ring still has the driver as a reader/writer). The /stub-driver source documents this explicitly.
3. **`SYS_SPAWN_MAX_FDS = 16`.** Comfortable for the v1.0 use case (joey passes 2; per-user stratumd takes 2-3; corvus takes 1-2). Bump on demand — the kernel-stack scratch grows linearly with the cap.
4. **No close-on-exec yet.** Plan 9's exec preserves all fds (only close-on-exec'd are dropped). Thylacine v1.0 exec_setup preserves the handle table as-is. When close-on-exec lands, this syscall continues to work — the inherited fds are explicitly marked as not-close-on-exec.
