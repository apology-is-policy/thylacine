# 52. SYS_PIPE — pipe(fd[2]) syscall (P5-fd-pipe)

The first userspace consumer of the kernel pipe primitive: a SVC handler that creates a connected Spoor pair via `pipe_create()` and installs both as KOBJ_SPOOR handles in the caller's HandleTable.

Per ARCH §10.3 + §11.2. The userspace surface is `pipe(fd[2])`; the kernel returns the two file descriptors via two registers (x0 = read-end, x1 = write-end).

---

## Purpose

Until this chunk, `kernel/pipe.c` was kernel-internal — no userspace surface. SYS_PIPE is the first syscall that:
1. Allocates new Spoors (via `pipe_create()`).
2. Installs them as KOBJ_SPOOR handles in the calling Proc's HandleTable.
3. Wires the KOBJ_SPOOR release path so closing the handle (or the Proc exiting) tears down the Spoor end-to-end.

Together this lets userspace programs eventually:
- `pipe(fd[2])` to obtain a read-end + write-end (this chunk).
- read/write/close them (future P5-fd-rw + P5-fd-close).
- dup them, walk-transfer them across 9P sessions, mount them somewhere (future chunks).

---

## ABI

```
SYS_PIPE = 8
No arguments.
Returns:
    x0 = read-end fd (hidx_t)         on success
    x1 = write-end fd (hidx_t)        on success
    x0 = -1                            on failure (x1 unmodified)
```

The handler writes both fds directly into the exception_context's `regs[0]` and `regs[1]` slots; no user-VA buffer is required. This is the same pattern POSIX promises for `pipe(2)` when the syscall ABI carries two return registers (Linux returns the rest via the buffer; Thylacine's ABI per ARCH §11.6 reserves x1 for the second return value).

### Rights granted

Both handles get `RIGHT_READ | RIGHT_WRITE | RIGHT_TRANSFER`. The wrong-end guards inside `devpipe_read` / `devpipe_write` (which look at the endpoint's `is_read_end` flag) provide the actual gating — handle rights are an additional gate, not the primary one. Future iterations may pre-narrow rights per end (read-only on the read end; write-only on the write end) once we settle on the rights-tightening policy.

---

## Userspace API — `<thyla/syscall.h>`

```c
__attribute__((always_inline))
static inline long t_pipe(long *out_rd_fd, long *out_wr_fd) {
    register long x0 __asm__("x0");
    register long x1 __asm__("x1");
    register long x8 __asm__("x8") = T_SYS_PIPE;
    __asm__ volatile (
        "svc #0"
        : "=r"(x0), "=r"(x1)
        : "r"(x8)
        : "memory", "cc"
    );
    if (x0 < 0) return -1;
    *out_rd_fd = x0;
    *out_wr_fd = x1;
    return 0;
}
```

Returns 0 on success with `*out_rd_fd` / `*out_wr_fd` populated. Returns -1 on failure.

---

## Implementation

`kernel/syscall.c::sys_pipe_handler` + `kernel/syscall.c::sys_pipe_for_proc`. The latter is the non-static inner used by kernel-internal tests; the static handler wraps it with `current_thread()` lookup.

### Handler

```c
int sys_pipe_for_proc(struct Proc *p, hidx_t *out_rd, hidx_t *out_wr) {
    if (!p || !out_rd || !out_wr)    return -1;

    struct Spoor *rd = NULL, *wr = NULL;
    if (pipe_create(&rd, &wr) < 0)   return -1;

    rights_t r = RIGHT_READ | RIGHT_WRITE | RIGHT_TRANSFER;

    hidx_t fd_rd = handle_alloc(p, KOBJ_SPOOR, r, rd);
    if (fd_rd < 0) {
        spoor_clunk(rd);
        spoor_clunk(wr);
        return -1;
    }
    hidx_t fd_wr = handle_alloc(p, KOBJ_SPOOR, r, wr);
    if (fd_wr < 0) {
        // First handle owns rd via handle_release_obj's spoor_clunk.
        handle_close(p, fd_rd);
        spoor_clunk(wr);
        return -1;
    }

    *out_rd = fd_rd;
    *out_wr = fd_wr;
    return 0;
}
```

Rollback discipline: on second-handle-alloc failure, the first handle is closed (which the new KOBJ_SPOOR release path turns into `spoor_clunk(rd)`); the second Spoor is clunk'd directly (never reached the table).

### KOBJ_SPOOR wired into handle release / acquire

`kernel/handle.c::handle_release_obj` now `spoor_clunk`s for KOBJ_SPOOR. `handle_acquire_obj` (used by dup) `spoor_ref`s. Together these mean:

- **`handle_close(p, fd)`** on a KOBJ_SPOOR runs `spoor_clunk(obj)` → drops one ref → if last ref, frees the Spoor.
- **`proc_free(p)`** runs `handle_table_free`, which walks every slot and calls `handle_release_obj`. KOBJ_SPOOR slots get `spoor_clunk`'d → both pipe ends released → ring's per-endpoint ref drops to 0 → ring freed.
- **`handle_dup(p, oldfd)`** would call `handle_acquire_obj` → `spoor_ref(obj)` → second handle has its own ref; future `handle_close` on either end releases that one ref independently.

This is the first kobj kind to be reference-counted on the handle table beyond KOBJ_BURROW / hw kinds. The discipline mirrors the same `acquire / release` pattern those use.

---

## Tests

3 kernel-internal tests in `kernel/test/test_sys_pipe.c`:

| Test | Covers |
|---|---|
| `sys_pipe.allocates_two_distinct_spoor_handles` | Returns 0; two distinct fds; both KOBJ_SPOOR with rights READ\|WRITE\|TRANSFER; `obj` pointers distinct; `spoor_total_allocated` += 2. |
| `sys_pipe.proc_free_releases_handles` | Calling `proc_free` on a Proc holding two pipe handles releases the ring + both Spoors. `pipe_total_freed` += 1; `spoor_total_freed` += 2. **This is the end-to-end test of the new KOBJ_SPOOR release path.** |
| `sys_pipe.handle_close_releases_one_end` | Closing one handle drops the ring's per-end ref but keeps the ring alive (other end still holds a ref); closing the second frees the ring. |

Tests call `sys_pipe_for_proc(p, ...)` (the non-static inner) with a `proc_alloc`'d test Proc — bypassing the SVC entry path. The SVC layer is mechanical wrapping over `sys_pipe_for_proc`; the substance is exercised here.

---

## Error paths

- `sys_pipe_for_proc` returns -1 on:
  - NULL Proc / NULL out pointer.
  - `pipe_create` OOM (ring or endpoint allocation fails).
  - `handle_alloc` failure (table full; ≥ PROC_HANDLE_MAX = 64 active handles).
- On any failure all partial state is cleaned up. Both Spoors are clunked; any installed handle is closed (which clunks its Spoor too).

---

## Status

| Component | State |
|---|---|
| SYS_PIPE handler (kernel/syscall.c) | **Landed (P5-fd-pipe)** |
| KOBJ_SPOOR release/acquire wiring (kernel/handle.c) | **Landed (P5-fd-pipe)** |
| Syscall dispatcher entry | **Landed (P5-fd-pipe)** |
| 3 kernel-internal integration tests | **Landed (P5-fd-pipe)** |
| Userspace `t_pipe()` stub in libt | Deferred to P5-fd-rw |
| Userspace `/pipe-probe` binary | Deferred to P5-fd-rw |
| SYS_READ / SYS_WRITE / SYS_CLOSE / SYS_DUP | Deferred to P5-fd-rw / P5-fd-dup |

---

## Known caveats / footguns

### No user-VA fd[2] buffer

The handler returns both fds via x0 + x1, not via a user-VA fd[2] pointer. This sidesteps adding a new uaccess primitive (uaccess_store_u32 doesn't yet exist; only uaccess_load_u8 is wired). Userspace `t_pipe()` stub captures both registers.

POSIX `pipe(2)` writes to a user buffer; the Thylacine libc shim (Phase 6+) will adapt by storing x0 + x1 into the user-supplied buffer in userspace.

### Per-handle rights aren't pre-narrowed

Both handles get RIGHT_READ | RIGHT_WRITE | RIGHT_TRANSFER. The actual read/write gating happens inside the Dev vtable (`devpipe_read` / `devpipe_write` check `is_read_end`). Future revisions may pre-narrow rights to RIGHT_READ on the read end and RIGHT_WRITE on the write end; deferred until the rights-tightening policy across the syscall surface is settled.

### Table-full → -1, not specific errno

`handle_alloc` returns -1 on table-full / OOM / bad rights. The SYS_PIPE handler propagates -1 without distinguishing. The Phase 5+ syscall ABI extension (per-thread errstr) will let callers distinguish; v1.0 returns -1 only.

### No `pipe(2)` POSIX-style semantics yet

Without SYS_READ / SYS_WRITE / SYS_CLOSE syscalls, userspace can `pipe()` but can't actually use the fds. Phase 5+ chunks fill in:
- `P5-fd-rw`: SYS_READ / SYS_WRITE handlers (each lookup KOBJ_SPOOR handle, validate user-VA buffer, route to dev->read/write).
- `P5-fd-close`: SYS_CLOSE handler (handle_close on the fd; routes through KOBJ_SPOOR release → spoor_clunk).
- `P5-fd-dup`: SYS_DUP handler (handle_dup; spoor_ref via KOBJ_SPOOR acquire path).

---

## Naming rationale

`SYS_PIPE` = 8, appending to the existing `SYS_*` enumeration. The name matches POSIX `pipe(2)` for familiarity; the internal name (`sys_pipe_handler`) mirrors the project convention of `sys_<verb>_handler`.

---

## Reference

- ARCH §10.3 (Pipes); §11.2 (Core syscalls).
- `docs/reference/51-pipe.md` (the underlying primitive).
- `docs/reference/19-handles.md` (handle table + KOBJ kinds).
- `specs/handles.tla` (handle invariants pinned by HandleAlloc + HandleClose).
- ROADMAP §7.3 (Phase 5 deliverables; P5-fd-pipe section).
