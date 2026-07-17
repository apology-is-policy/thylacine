# 53. SYS_READ / SYS_WRITE — byte I/O over fds (P5-fd-rw)

The two SVC handlers that let userspace actually exchange bytes through a `KOBJ_SPOOR` fd. Per ARCH §11.2: `read(fd, buf, n)` / `write(fd, buf, n)`. Each routes through the underlying Spoor's Dev vtable (`dev->read` / `dev->write`) after validating the user-VA buffer + the handle's rights.

Lands alongside `uaccess_store_u8` — the symmetric primitive to the existing `uaccess_load_u8` — which lets the kernel write a single byte back to user-VA with fault-fixup recovery.

---

## ABI

```
SYS_READ  = 9
SYS_WRITE = 10

Args (both):
    x0 = fd     (hidx_t)
    x1 = buf_va (user-VA pointer)
    x2 = len    (bytes)

Return:
    x0 = bytes transferred (>=0); 0 on EOF (read only); -1 on error.
```

Per-call cap: `SYS_RW_MAX = 128 KiB` (CF-3 A; was 4096). Userspace still loops for larger transfers — short reads/writes remain normal, and a single 9P RPC's payload is additionally clamped by the negotiated msize. The staging is two-tier: ops ≤ `SYS_RW_STACK` (4096, matches `PIPE_BUF_SIZE`) use the kernel-stack scratch exactly as before; larger ops take a transient `kmalloc` bounce (freed before return), degrading to the stack tier on allocation failure so memory pressure shortens an op rather than failing it.

### Rights gates

- SYS_READ requires `RIGHT_READ` on the handle.
- SYS_WRITE requires `RIGHT_WRITE`.

SYS_PIPE (P5-fd-pipe) grants `RIGHT_READ | RIGHT_WRITE | RIGHT_TRANSFER` on both ends, so a freshly-pipe()'d fd passes both gates. Future iterations may pre-narrow rights per end; the gate is the structural defense for that future.

### User-VA validation

`buf_va` and `buf_va + len` must both lie strictly within `[0, UACCESS_USER_VA_TOP)`. The bound (`1ull << 47`) is the same as SYS_PUTS uses (R12-uaccess F210). Overflow on `buf_va + len` is rejected.

`len == 0` is a no-op success (returns 0) — but the handle is still validated to match POSIX-style discipline (bad fd returns -1 regardless of length).

---

## Implementation

`kernel/syscall.c::sys_read_handler` + `sys_write_handler`. Each factored into a non-static `sys_{read,write}_for_proc(p, h, kbuf, len)` inner used by kernel-internal tests, plus the static SVC handler that wraps the inner with user-VA validation + bounce-buffer copy.

### `sys_write_handler`

1. Resolve the calling Proc via `current_thread()`.
2. Validate the user-VA range: NULL / `>= UACCESS_USER_VA_TOP` / overflow rejected.
3. Cap `len` at `SYS_RW_MAX` (128 KiB).
4. For `len == 0`: just validate the fd (handle exists + RIGHT_WRITE) and return 0.
5. Otherwise pick the staging tier: `len <= SYS_RW_STACK` → the 4 KiB stack scratch; larger → a transient `kmalloc(len)` (on allocation failure, `len` degrades to `SYS_RW_STACK` and the stack scratch serves — a short write, never a failed one).
6. `uaccess_copy_in(scratch, buf_va, len)` — the bulk fault-fixup copy (CF-3 A; replaced the per-byte `uaccess_load_u8` loop). On fault → free the bounce, return -1 (no bytes delivered).
7. Call `sys_write_for_proc(p, fd, scratch, len)`; free the bounce.

`sys_write_for_proc`:
1. Look up the handle: `KOBJ_SPOOR` kind, `RIGHT_WRITE` set.
2. Get the Spoor; verify `dev->write` exists.
3. Call `dev->write(spoor, kbuf, len, spoor->offset)`.
4. Advance `spoor->offset` by the returned byte count.
5. Return the byte count.

### `sys_read_handler`

Mirror image: validate, look up handle (RIGHT_READ), call `dev->read` into a stack scratch, copy scratch back to user-VA via `uaccess_store_u8`. Returns bytes read, 0 on EOF, -1 on error.

Partial-fault behavior on the user-VA store loop: returns -1. Bytes already written into user-VA up to the fault remain; bytes drained from `dev->read` beyond the fault are LOST (data-loss caveat — Phase 5+ can pre-touch pages to mitigate).

### `uaccess_store_u8` (new at P5-fd-rw)

The symmetric primitive to `uaccess_load_u8`. `arch/arm64/uaccess.S`:

```asm
uaccess_store_u8:
uaccess_store_u8_op:
    strb    w1, [x0]                // FAULT POINT
    mov     x0, xzr
    ret
uaccess_store_u8_fault:
    mov     x0, #-1
    ret
```

Plus a fixup-table entry (`.uaccess_fixup` section) mapping `_op` → `_fault`. The existing fault dispatcher (`arch/arm64/exception.c`) already handles kernel-mode user-VA faults for both reads and writes (via `userland_demand_page`, which itself checks `VMA_PROT_WRITE` against the VMA's prot for write faults). No dispatcher change needed.

C declaration in `arch/arm64/uaccess.h`:

```c
extern s64 uaccess_store_u8(u64 user_va, u8 value);
```

Returns 0 on success / -1 on fault. Identical contract to `uaccess_load_u8`.

---

## Tests

4 new kernel-internal tests appended to `kernel/test/test_sys_pipe.c` (kept in the same file as SYS_PIPE since they share the test_proc + sys_pipe_for_proc fixture):

| Test | Covers |
|---|---|
| `sys_rw.write_then_read_round_trip` | Write 7 bytes through `sys_write_for_proc(fd_wr, ...)`; read them back through `sys_read_for_proc(fd_rd, ...)`. FIFO order preserved. **The canonical end-to-end test of the byte I/O path.** |
| `sys_rw.rights_check` | Out-of-range fd (9999) returns -1 from both handlers. (Rights-reduction can't be tested without a `sys_handle_reduce` syscall, deferred.) |
| `sys_rw.zero_length_validates_fd` | Zero-length read/write on a valid fd → 0; on a bad fd → -1. POSIX-discipline behavior. |
| `sys_rw.read_after_close_returns_eof` | Close the write end; subsequent read on the empty pipe with `write_eof` set returns 0 (EOF) immediately without blocking. **Composes SYS_READ + the blocking-pipe EOF protocol (`specs/pipe.tla::ReadEof`).** |

Tests use `sys_*_for_proc` directly (bypassing the SVC entry + user-VA copy). The user-VA path itself is exercised by SYS_PUTS's existing tests + by the future userspace probe binaries.

Test count 393 → 397. 397/397 PASS × default + UBSan.

---

## Spec posture

No new TLA+ module. Pure composition:

- `specs/handles.tla` — HandleAlloc + HandleClose; rights monotonicity carries.
- `specs/pipe.tla` — wait/wake protocol for pipes; SYS_READ on empty + `write_eof` matches `ReadEof`; SYS_WRITE on full + `read_eof` matches `WriteEpipe`. The actual blocking would compose with `specs/scheduler.tla::NoMissedWakeup` if a user-mode test exercised it.

The byte I/O path itself has no new invariants: it's a thin shim from syscall ABI to Dev vtable.

---

## Audit posture

Extends:
- `kernel/syscall.c` (in the audit-trigger list since P4-Ib).
- `arch/arm64/uaccess.S` / `.c` (the uaccess surface). Added `uaccess_store_u8` with its fixup-table entry — same shape + invariants as `uaccess_load_u8`.

New attack surfaces:
1. **User-VA store can write to attacker-controlled location** (within `[0, UACCESS_USER_VA_TOP)`). The VMA write-permission check inside `userland_demand_page` blocks writes to read-only mappings. No additional kernel-mode privilege escalation surface introduced.
2. **Two-tier scratch** (CF-3 A): 4 KiB on the kernel stack (`SYS_RW_STACK`) for small ops; a transient `kmalloc` bounce up to `SYS_RW_MAX` (128 KiB) for bulk ops, freed on every path. The heap tier is user-drivable transient kernel memory, and "transient" is attacker-delayable: a blocked `dev->read`/`write` (a held-open pipe, an idle `/net` socket, a hung server) holds its bounce for the block's duration, and the allocation is order-5 (buddy-fragmentation-hostile). The CF-3 audit (F1) therefore added the **per-Proc bounce budget**: `sys_bounce_charge`/`uncharge` against `Proc.bounce_bytes`, capped at `PROC_BOUNCE_MAX` (512 KiB = four concurrent bulk ops; ample for the measured 84%-depth-1 build workloads); over-budget ops degrade to the stack tier (a short op, never a failure). `PRINCIPAL_SYSTEM` is exempt (the I-32 TCB pattern). The honest residual bound: a fork tree still aggregates per-Proc budgets (the I-32 per-Proc shape, same as `page_count`), capped by the child/thread axes.

The `uaccess_store_u8` fault path is structurally identical to `uaccess_load_u8` (same fixup-table format, same dispatcher). No new fault-handling logic.

---

## Performance characteristics

CF-3 A landed the u64-stride bulk primitives this section originally deferred: `uaccess_copy_in` / `uaccess_copy_out` (arch/arm64/uaccess.S — byte head to 8-align the user VA, 8-byte body, byte tail; three fixup entries each sharing one fault label; see 40-uaccess.md). The benchmark that justified it: the CF-3 Tread-stream measurement — 67% of a go build's read RPCs were exactly-4096 userspace chunks (the 4 KiB cap, not the 32 KiB msize, was the binding constraint on bulk read throughput ≈ 27 MiB/s), and the per-byte copy loop was a function call per byte on every op.

---

## Status

| Component | State |
|---|---|
| `SYS_READ` + `SYS_WRITE` handlers | **Landed (P5-fd-rw)** |
| `sys_{read,write}_for_proc` inners | **Landed (P5-fd-rw)** |
| `uaccess_store_u8` primitive + fixup entry | **Landed (P5-fd-rw)** |
| Syscall dispatcher entries | **Landed (P5-fd-rw)** |
| 4 kernel-internal integration tests | **Landed (P5-fd-rw)** |
| Userspace `t_read` / `t_write` stubs in libt | Deferred to P5-fd-pipe-probe |
| Userspace probe binary | Deferred to P5-fd-pipe-probe |
| Larger-stride `copy_from_user` / `copy_to_user` | Deferred (future P5-fd-rw-uaccess-fast) |

---

## Known caveats / footguns

### Per-call cap = SYS_RW_MAX (128 KiB since CF-3 A)

Larger transfers require userspace to loop (the libc shim + the go port already do). A single 9P RPC still carries at most the negotiated msize's payload (32,757 B today), so bulk reads return short at that boundary until the CF-3 B transport lift lands. `SYS_READDIR` / `SYS_GETRANDOM` / `SYS_EXPLICIT_BZERO` deliberately keep the 4 KiB bound (`SYS_RW_STACK`).

### Partial-fault data loss on SYS_READ

If `uaccess_store_u8` faults mid-copy, the bytes already drained from the Spoor's `dev->read` but not yet written to user-VA are LOST. The kernel returns -1 without indicating the partial drain. Documented; Phase 5+ can pre-touch user pages to mitigate.

### Stack-allocated bounce buffer (small tier)

4 KiB on the kernel test thread's 16 KiB stack is borderline. Real syscalls run on the per-Proc kernel stack which is the same size. The 12 KiB headroom is sufficient for v1.0 syscall depths but should be revisited when the call graph deepens. CF-3 A deliberately did NOT grow this: the bulk tier lives on the heap precisely so the stack frame stays at the audited 4 KiB.

### `dev->offset` advance is unguarded

The handler advances `spoor->offset += n` after a successful `dev->{read,write}`. For pipes, offset is meaningless. For seekable devs (future), the offset advance is correct. No locking around the offset advance — at v1.0 single-thread per Spoor is enforced by the Proc's call discipline; multi-thread access to the same fd (e.g., dup'd handles) would race. Phase 5+ adds per-Spoor or per-Handle synchronization.

### Zero-length operations still validate the fd

POSIX-style: `write(bad_fd, NULL, 0)` returns `-1 (EBADF)`. The handler matches by validating the handle before short-circuiting on `len == 0`.

### No partial-success return code

SYS_READ / SYS_WRITE return either the byte count or -1; there's no way to communicate "we got partial bytes but then faulted." This is the POSIX shape for fault-style failures. The libc shim translates -1 into errno + leaves the caller's buffer in a documented "indeterminate" state on EFAULT.

---

## Naming rationale

`SYS_READ` = 9, `SYS_WRITE` = 10 — append to the existing `SYS_*` enumeration. Matches POSIX for familiarity. Internal names mirror the project convention (`sys_<verb>_handler` + `sys_<verb>_for_proc`).

`uaccess_store_u8` symmetric with `uaccess_load_u8`. The `_u8` width suffix is preserved so larger primitives (`_u32`, `_u64`, `copy_*`) can land later without renaming.

---

## Reference

- ARCH §11.2 (Core syscalls).
- `docs/reference/52-sys-pipe.md` (SYS_PIPE — the chunk that introduced KOBJ_SPOOR fd usage from userspace).
- `docs/reference/51-pipe.md` (the underlying pipe primitive).
- `docs/reference/19-handles.md` (handle table + rights model).
- `arch/arm64/uaccess.h` (kernel-side uaccess primitives).
- `specs/pipe.tla` (the wait/wake protocol composed with SYS_READ's EOF path).
- ROADMAP §7.3 (Phase 5 deliverables; P5-fd-rw section).
