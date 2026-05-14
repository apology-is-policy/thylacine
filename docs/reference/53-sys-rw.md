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

Per-call cap: `SYS_RW_MAX = 4096` bytes (matches `PIPE_BUF_SIZE`). Userspace loops for larger transfers. The cap is the kernel-side bounce-buffer size; raising it would require either growing the stack frame past safety bounds or routing through heap.

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
3. Cap `len` at `SYS_RW_MAX`.
4. For `len == 0`: just validate the fd (handle exists + RIGHT_WRITE) and return 0.
5. Otherwise: allocate a 4 KiB stack scratch (`u8 scratch[SYS_RW_MAX]`).
6. Loop: `uaccess_load_u8(buf_va + i, &scratch[i])` for `i = 0..len`. On any fault → return -1 (partial scratch is dropped; no bytes delivered).
7. Call `sys_write_for_proc(p, fd, scratch, len)`.

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
2. **Stack scratch buffer** (4 KiB on the kernel stack). Bounded by `SYS_RW_MAX`; can't exceed the bounds.

The `uaccess_store_u8` fault path is structurally identical to `uaccess_load_u8` (same fixup-table format, same dispatcher). No new fault-handling logic.

---

## Performance characteristics

Per-byte loops are not the fastest possible — production uses larger `copy_from_user` / `copy_to_user` primitives (u64-stride). v1.0 uses single-byte loops because:
1. The existing `uaccess_load_u8` already operates byte-wise (matches SYS_PUTS).
2. Performance budget at v1.0 P5 isn't tight enough to require wider strides.
3. Larger-stride primitives need separate fixup-table entries + per-stride fault recovery; each adds audit-bearing surface.

A future `P5-fd-rw-uaccess-fast` chunk can land u64-stride `copy_from_user` / `copy_to_user` once a benchmark justifies it.

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

### Per-call cap = 4096 bytes

Larger transfers require userspace to loop. The libc shim (Phase 6+) will loop transparently for POSIX `read(2)` / `write(2)` semantics.

### Partial-fault data loss on SYS_READ

If `uaccess_store_u8` faults mid-copy, the bytes already drained from the Spoor's `dev->read` but not yet written to user-VA are LOST. The kernel returns -1 without indicating the partial drain. Documented; Phase 5+ can pre-touch user pages to mitigate.

### Stack-allocated bounce buffer

4 KiB on the kernel test thread's 16 KiB stack is borderline. Real syscalls run on the per-Proc kernel stack which is the same size. The 12 KiB headroom is sufficient for v1.0 syscall depths but should be revisited when the call graph deepens.

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
