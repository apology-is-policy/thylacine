# 57. /attach-probe — userspace integration test of the Phase 5 mount surface (P5-attach-probe)

End-to-end userspace integration of SYS_ATTACH_9P + SYS_MOUNT + SYS_UNMOUNT driven through a real EL0 Proc against a kernel-thread 9P responder. The userspace companion to `kernel/test/test_sys_mount.c`'s kernel-internal coverage: this binary exercises the actual SVC dispatch path and the wire codec composed over real pipe Spoors.

---

## Purpose

`test_sys_mount.c` covers the handler logic from the kernel side via `sys_mount_for_proc` / `sys_unmount_for_proc` inners. `/attach-probe` covers what those inners can't:

- The SYS_ATTACH_9P / SYS_MOUNT / SYS_UNMOUNT SVC entry path at EL0 — userspace issues `svc #0` with the syscall number in x8 and the args in x0..x4, the exception-vector routes to `syscall_dispatch`.
- The libt inline-asm stubs (`t_attach_9p` / `t_mount` / `t_unmount`) compile + dispatch correctly.
- The 9P codec + session + transport + dev9p layers compose end-to-end against real Spoor I/O, not the loopback-fn shortcut used by the lower-layer unit tests.
- The full lifecycle: open transport → attach → mount → unmount → close attach → close transport — runs in user space without intervention.

This chunk closes the "userspace integration test" follow-up flagged at P5-attach-syscall.

---

## Architecture

Why a kernel-thread 9P responder + single userspace Proc rather than two cooperating userspace Procs (as the original ROADMAP §7.2 sketch suggested):

- v1.0 `rfork` supports `RFPROC` only. `RFFDG` (shared fd table — Plan 9 idiom) is deferred.
- Cross-Proc Spoor transfer is gated on `RIGHT_TRANSFER` through a 9P session per ARCH I-4 — but we'd need a session for that, which is what we're constructing. Circular dependency.
- A kernel-thread responder sidesteps both constraints. When `RFFDG` lands later, the test can graduate to a two-proc design.

```
                     ┌─────────────────────────────────────────────┐
                     │   userspace probe Proc                      │
                     │                                              │
                     │   fd 0 (KOBJ_SPOOR) — c2s_wr (tx)            │
                     │   fd 1 (KOBJ_SPOOR) — s2c_rd (rx)            │
                     │                                              │
                     │   t_attach_9p(0, 1, "/", 1, 0)               │
                     │   t_mount(attach_fd, 99, 0)                  │
                     │   t_unmount(99)                              │
                     │   t_close(attach_fd) + t_close(0) + t_close(1) │
                     │   t_putstr("attach-probe: PASS\n")           │
                     │   t_exits(0)                                  │
                     └────────────┬─────────────────────┬───────────┘
                                  │                     │
                                  ▼ writes              ▲ reads
                         ┌──────────────────────────────────────────┐
                         │ c2s ring (4 KiB)         s2c ring (4 KiB) │
                         └──────────────────────────────────────────┘
                                  ▲                     │
                                  │ reads               ▼ writes
                     ┌────────────┴─────────────────────┴───────────┐
                     │ kernel-thread responder (kproc)              │
                     │   rx = c2s_rd; tx = s2c_wr.                  │
                     │   Loop:                                       │
                     │     - read 4-byte frame size.                │
                     │     - read body.                              │
                     │     - build response (Tversion → Rversion,    │
                     │       Tattach → Rattach, * → Rlerror).        │
                     │     - write response.                         │
                     │   Exit on rx EOF (probe closed tx_fd).        │
                     └──────────────────────────────────────────────┘
```

### Refcount discipline

Each `pipe_create` returns Spoors at `ref=1`. The four Spoors are TRANSFERRED to their owners without bumping:

- `c2s_rd` → responder thread's static ctx. Responder owns this ref. At loop exit, `spoor_clunk` drops it.
- `s2c_wr` → responder thread's static ctx. Same.
- `c2s_wr` → probe's handle table fd 0. `handle_alloc` consumes the ref (KOBJ_SPOOR slot owns it).
- `s2c_rd` → probe's handle table fd 1. Same.

Boot retains NO refs after setup. This is load-bearing for EOF propagation under Plan-9 cclose: if boot held a residual ref, the probe's `t_close(tx_fd)` would drop to 1 (not zero) → `devpipe_close` wouldn't fire → write_eof not set → responder blocks forever in `dev_read`. An earlier draft of the test surfaced this hang; the per-Spoor "transfer not bump" discipline is the fix.

### Per-Spoor ref count over time

| Event | c2s_wr | c2s_rd | s2c_wr | s2c_rd |
|---|---|---|---|---|
| `pipe_create` × 2 | 1 | 1 | 1 | 1 |
| Hand to responder | 1 | 1 | 1 | 1 |
| `rfork` + thunk `handle_alloc` (consumes refs) | 1 | 1 | 1 | 1 |
| probe `t_attach_9p` → SYS_ATTACH_9P spoor_ref tx + rx | 2 | 1 | 1 | 2 |
| probe `t_mount` (no Spoor ops) | 2 | 1 | 1 | 2 |
| probe `t_unmount` (drops dev9p Spoor only) | 2 | 1 | 1 | 2 |
| probe `t_close(attach_fd)` → dev9p_close → spoor_clunk tx + rx | 1 | 1 | 1 | 1 |
| probe `t_close(fd 0)` → spoor_clunk(c2s_wr) → LAST DROP → devpipe_close (write_eof=true; wakeup responder) | 0 (freed) | 1 | 1 | 1 |
| probe `t_close(fd 1)` → spoor_clunk(s2c_rd) → LAST DROP → devpipe_close (read_eof=true; wakeup) | — | 1 | 1 | 0 (freed) |
| probe `t_exits(0)` (handle_table_free no-ops; all fds closed) | — | 1 | 1 | — |
| responder reads EOF → loop exit | — | 1 | 1 | — |
| responder spoor_clunk(rx=c2s_rd) → LAST DROP → devpipe_close (sets c2s read_eof; ring freed) | — | 0 (freed) | 1 | — |
| responder spoor_clunk(tx=s2c_wr) → LAST DROP → devpipe_close (sets s2c write_eof; ring freed) | — | — | 0 (freed) | — |

All four Spoors + both rings are torn down with zero leakage.

---

## The 9P responder

Embedded in `kernel/test/test_attach_probe.c`. Handles three message types:

- **Tversion (100)** — returns `Rversion` with `msize=4096`, version="9P2000.L".
- **Tattach (104)** — returns `Rattach` with root qid `{type=QTDIR, version=0, path=1}`.
- **Tclunk (120)** — returns `Rclunk` (empty body).
- **Anything else** — returns `Rlerror` with `ecode=5` (EIO) so the probe surfaces a clean failure on protocol drift.

Buffer sizes: 256-byte request + response stacks. Sufficient for the bring-up subset (max ~24 bytes per message in this scenario).

### What Tclunk reveals

Although the responder handles Tclunk, the probe doesn't actually trigger one. The probe's `t_close(attach_fd)` runs `dev9p_close` → `p9_attached_destroy` → `p9_client_clunk(client, root_fid)`. But `p9_session_send_clunk` **rejects clunking root_fid by design** (see `kernel/9p_session.c::p9_session_send_clunk` line 303: `if (fid == s->root_fid) return -1;`). Root fids are session-scoped; they get implicitly released at session close (`p9_session_close` is "no wire op" per spec). `p9_attached_destroy` ignores the rejected return value for that reason.

The responder's message count is therefore exactly 2 (Tversion + Tattach), not 3 as one might initially expect. The chunk's test asserts `>= 2` and documents the design intent in `kernel/test/test_attach_probe.c` next to the assertion.

---

## The probe binary

`usr/attach-probe/attach-probe.c`. ~14.9 KiB ELF. Calling convention:

- Expects fd 0 (tx) + fd 1 (rx) pre-installed in the handle table — the kernel test framework does this in the `rfork` thunk before `exec_setup`.
- No `t_pipe` call — those handles arrive from outside.
- Standard libt + inline-asm syscall stubs.

Sequence (all errors → diagnostic + exit 1):

```c
attach_fd = t_attach_9p(0, 1, "/", 1, 0)  // Tversion + Tattach
t_mount(attach_fd, 99, 0)                   // territory mount table entry
t_unmount(99)                               // remove the entry
t_unmount(99)                               // should fail (already unmounted) — regression coverage
t_close(attach_fd)                          // dev9p_close → attempted Tclunk (rejected internally)
t_close(0)                                  // close tx → write_eof on c2s
t_close(1)                                  // close rx → read_eof on s2c
t_putstr("attach-probe: PASS\n")
t_exits(0)
```

---

## Build wiring

- `usr/attach-probe/attach-probe.c` + `usr/attach-probe/CMakeLists.txt`.
- `usr/CMakeLists.txt` — `add_subdirectory(attach-probe)`.
- `tools/build.sh` — `usr_bins=( "hello" "pipe-probe" "attach-probe" )`.
- `kernel/CMakeLists.txt` — `test/test_attach_probe.c` in `KERNEL_SRCS`.
- `kernel/test/test.c` — extern decl + registry entry `userspace.attach_probe_round_trip`.

The probe is loaded from the ramfs cpio at runtime — the kernel test calls `devramfs_lookup("attach-probe", ...)` and skips (rather than fails) if the binary isn't present. This lets `tools/build.sh kernel` (kernel-only build, no userspace artifacts) still pass.

---

## Tests

Single registry entry: `userspace.attach_probe_round_trip`. Verifies:

1. `pipe_create` × 2 succeeds.
2. Responder thread + probe Proc both spawn.
3. Probe exits with status 0 (= internal probe checks all passed: `attach-probe: PASS` on UART).
4. Responder finishes cleanly after probe exits (no hang, no kernel state poisoning).
5. Responder handled at least 2 messages (Tversion + Tattach).

Boot log markers:

```
/attach-probe size=14968 bytes → setting up responder + rfork + exec
attach-probe: PASS
/attach-probe reaped pid=... status=0 responder_msgs=2 — SYS_ATTACH_9P + SYS_MOUNT + SYS_UNMOUNT verified end-to-end
```

---

## Error paths

| Probe-side | Trigger |
|---|---|
| `t_attach_9p < 0` | wire codec / responder / session error |
| `t_mount < 0` | mount-table full or rights mismatch (shouldn't trigger here) |
| `t_unmount < 0` (first) | path not mounted (shouldn't trigger) |
| `t_unmount >= 0` (second) | path STILL mounted after first unmount (would be a bug) |
| `t_close(attach_fd) != 0` | bad fd or kernel state corruption |
| `t_close(0/1) != 0` | bad fd |

Each probe-side error path emits a diagnostic via `t_putstr` and `return 1` → `t_exits(1)` → boot reaps with `status=1` → test framework reports the failure.

Kernel-side:

| Symptom | Trigger |
|---|---|
| Responder loop never started | `thread_create` returned NULL (out of slot in kproc thread table) |
| Responder hangs in `dev_read` | EOF propagation broken — typically a residual ref on c2s_wr |
| `g_responder_finished == false` after 256 scheds | Same as above, or scheduler not picking the responder |
| `responder_msgs_handled < 2` | wire-level error (responder break mid-loop); look for `responder: ...` diagnostic prints |

---

## Performance characteristics

Boot-time addition: ~150ms in default build (one rfork + responder thread + 2 round-trip 9P exchanges + cleanup). Negligible compared to the multi-thread blocking tests' ~5s footprint.

UBSan adds ~3× overhead per the existing pattern; `BOOT_TIMEOUT=60` is sufficient.

---

## Status

| Item | State |
|---|---|
| Userspace probe binary | LANDED |
| Kernel-side responder + test framework | LANDED |
| Build wiring (usr/CMakeLists.txt, tools/build.sh, kernel/CMakeLists.txt) | LANDED |
| Two-userspace-Proc design (RFFDG-based) | Deferred — needs `rfork(RFFDG)` first |
| Tclunk wire coverage | Not achievable at v1.0 due to session-level root_fid clunk rejection; not a bug per the spec |

Landed at commit (P5-attach-probe substantive). Hash fixup follows.

---

## Known caveats / footguns

1. **Boot must NOT keep Spoor refs after setup.** Each `pipe_create` ref must be transferred (not bumped) to its eventual owner. A residual ref on `c2s_wr` would prevent `devpipe_close` on the probe's `t_close(tx_fd)`, which would prevent the responder's EOF, causing a hang.

2. **Responder thread parks via `for (;;) sched();`.** `exits()` extincts from kproc; bare return + WFE-spin in the trampoline wedges the CPU. The cooperative sched-loop is the v1.0 idiom (matches `test_pipe_blocking.c`'s consumer_*_entry pattern).

3. **The probe's fd numbers (0, 1) are hardcoded.** When `rfork(RFFDG)` lands and we move to a two-userspace-Proc design, the probe should call `t_pipe` itself rather than assume pre-installed fds.

4. **The kernel test relies on cooperative scheduling.** Boot polls `g_responder_finished` for up to 256 sched cycles. If the scheduler ever becomes truly preemptive without yielding semantics, this loop may need a more robust sync primitive (e.g., wait_on_thread).

5. **The responder is a kproc-resident thread that lives forever after the test.** Its Thread struct sits in the kproc thread list as a no-op spinner until kernel shutdown. Matches the `test_pipe_blocking.c` pattern; no leak across tests because kproc's table has plenty of slots.
