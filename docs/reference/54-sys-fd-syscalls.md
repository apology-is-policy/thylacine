# 54. SYS_CLOSE / SYS_DUP + /pipe-probe userspace test (P5-fd-syscalls)

The final two byte-I/O handlers (close + dup) plus the first userspace binary that exercises the entire fd round-trip end-to-end. Per ARCH §11.2: `close(fd)` / `dup(oldfd)` complete the POSIX-shaped byte-I/O syscall set. The userspace probe is the empirical evidence that the kernel's SVC + uaccess + dev-vtable + handle-table-release composition works through a real user-mode binary.

---

## ABI

```
SYS_CLOSE = 11
  Args:   x0 = fd (hidx_t)
  Return: x0 = 0 on success; -1 if fd invalid / already-closed.

SYS_DUP   = 12
  Args:   x0 = oldfd (hidx_t)
          x1 = new_rights (must be subset of oldfd's rights)
  Return: x0 = new fd (>=0) on success; -1 on bad oldfd / rights
          elevation / table-full / out-of-range rights bits.
```

Both handlers are thin wrappers over the existing `handle_close` / `handle_dup`. The substantive work happens in the kernel's per-kind acquire/release paths wired at P5-fd-pipe (KOBJ_SPOOR → spoor_ref / spoor_clunk).

---

## Userspace API — `<thyla/syscall.h>`

```c
extern inline long t_pipe(long *out_rd_fd, long *out_wr_fd);
extern inline long t_read(long fd, void *buf, size_t len);
extern inline long t_write(long fd, const void *buf, size_t len);
extern inline long t_close(long fd);
extern inline long t_dup(long oldfd, unsigned long new_rights);
```

Inline-asm stubs for each (`always_inline`). T_SYS_* numbers mirror the kernel header.

---

## Implementation — handlers

`kernel/syscall.c`:

```c
static s64 sys_close_handler(u64 hraw) {
    struct Thread *t = current_thread();
    if (!t)         return -1;
    struct Proc *p = t->proc;
    if (!p)         return -1;
    return (s64)handle_close(p, (hidx_t)hraw);
}

static s64 sys_dup_handler(u64 hraw, u64 new_rights_raw) {
    struct Thread *t = current_thread();
    if (!t)         return -1;
    struct Proc *p = t->proc;
    if (!p)         return -1;
    if (new_rights_raw & ~(u64)RIGHT_ALL)
                    return -1;
    hidx_t nh = handle_dup(p, (hidx_t)hraw, (rights_t)new_rights_raw);
    return (s64)nh;
}
```

`handle_dup` enforces the RightsCeiling invariant (`new_rights` ⊆ `oldfd.rights`) per `specs/handles.tla`. For KOBJ_SPOOR, the acquire path runs `spoor_ref` so each handle holds an independent reference.

---

## /pipe-probe — userspace integration test

`usr/pipe-probe/pipe-probe.c`: a 16 KiB ELF that exercises the full syscall surface end-to-end.

```c
int main(void) {
    long rd, wr;
    if (t_pipe(&rd, &wr) < 0) return 1;

    const char payload[] = "hello from /pipe-probe";
    if (t_write(wr, payload, sizeof(payload)-1) != sizeof(payload)-1)
        return 1;

    char got[64] = { 0 };
    if (t_read(rd, got, sizeof(got)) != sizeof(payload)-1) return 1;
    // verify bytes match

    long rd_dup = t_dup(rd, T_RIGHT_READ);
    if (rd_dup < 0 || rd_dup == rd) return 1;

    // Rights elevation must be rejected.
    if (t_dup(rd_dup, T_RIGHT_READ | T_RIGHT_WRITE) >= 0) return 1;

    if (t_close(rd_dup) != 0) return 1;
    if (t_close(rd)     != 0) return 1;
    if (t_close(wr)     != 0) return 1;

    // Double-close + use-after-close return -1.
    char dummy;
    if (t_read(rd, &dummy, 1) >= 0) return 1;
    if (t_close(rd) >= 0) return 1;

    t_putstr("pipe-probe: PASS\n");
    return 0;
}
```

The boot-log success marker `pipe-probe: PASS` is what `tools/test.sh` greps for (or what the kernel-side regression test verifies via `wait_pid`'s `status == 0`).

### Boot-log evidence

```
[test] userspace.pipe_probe_round_trip ...     /pipe-probe size=16552 bytes → rfork + exec
pipe-probe: PASS
    /pipe-probe reaped pid=1329 status=0 — full byte-I/O syscall surface verified end-to-end
```

This is the chunk where userspace pipe round-trip becomes a live empirical fact in the boot log. All prior pipe testing was kernel-internal scaffold.

---

## Tests

2 new tests + 1 new kernel-internal:

| Test | Covers |
|---|---|
| `sys_pipe.dup_spoor_handle_acquires_ref` | `handle_dup` of a KOBJ_SPOOR → KOBJ_SPOOR acquire path fires (`spoor_ref` bumps Spoor refcount). Dup'd handle is KOBJ_SPOOR with reduced rights pointing at same Spoor. Rights elevation rejected. `handle_close` on the dup drops the refcount back. |
| `userspace.pipe_probe_round_trip` | Spawns `/pipe-probe` via `rfork` + `exec_setup` + `userland_enter`. `wait_pid` reaps with status = 0. Full SVC + uaccess + dev-vtable + handle-table-release composition verified end-to-end through a real user-mode binary. |

The /pipe-probe binary itself runs **8 distinct syscall surfaces** during one invocation:
- SYS_PIPE (1×) — allocate handle pair.
- SYS_WRITE (1×) — bounce 22 bytes through uaccess_load_u8 + dev->write.
- SYS_READ (1×) — bounce 22 bytes through dev->read + uaccess_store_u8.
- SYS_DUP (1×) — handle alloc + KOBJ_SPOOR acquire path.
- SYS_DUP with rights-elevation (1×) — rejection path.
- SYS_CLOSE (3×) — handle release + KOBJ_SPOOR clunk path.
- SYS_READ on closed fd (1×) — rejection path.
- SYS_CLOSE on closed fd (1×) — rejection path.
- SYS_PUTS (1×) — emit the success marker.
- SYS_EXITS (1×) — clean exit with status 0.

Test count 397 → 399. 399/399 PASS × default + UBSan.

---

## Spec posture

No new TLA+ module. Pure composition over existing specs:
- `specs/handles.tla` — HandleClose / HandleDup actions; RightsCeiling invariant enforced.
- `specs/pipe.tla` — pipe wait/wake invariants propagate through SYS_READ / SYS_WRITE / SYS_CLOSE.

---

## Audit posture

Extends:
- `kernel/syscall.c` (SVC dispatch surface) — close + dup handlers added.
- `kernel/handle.c` (handle table) — no new code; existing KOBJ_SPOOR acquire/release wired at P5-fd-pipe is now exercised by userspace dup.

No new attack surfaces. The handlers are thin wrappers; rights enforcement is delegated to `handle_dup`'s existing RightsCeiling check.

The /pipe-probe binary is the first empirical test that combines:
- SVC entry from EL0.
- User-VA validation (`UACCESS_USER_VA_TOP` bound).
- uaccess_load_u8 / uaccess_store_u8 per-byte copy.
- Handle table lookup + rights check.
- Spoor / Dev / pipe blocking semantics.
- Handle release path (spoor_clunk on close).
- Handle acquire path (spoor_ref on dup).
- POSIX-shaped error returns.

If any layer regresses, /pipe-probe fails. It's the canonical regression test of the byte I/O composition.

---

## Status

| Component | State |
|---|---|
| SYS_CLOSE handler | **Landed (P5-fd-syscalls)** |
| SYS_DUP handler | **Landed (P5-fd-syscalls)** |
| Userspace libt stubs (t_pipe / t_read / t_write / t_close / t_dup) | **Landed (P5-fd-syscalls)** |
| /pipe-probe userspace test binary | **Landed (P5-fd-syscalls)** |
| Kernel test driver (`test_pipe_probe.c`) | **Landed (P5-fd-syscalls)** |
| KOBJ_SPOOR dup acquire-path verification | **Landed (P5-fd-syscalls)** |
| SYS_ATTACH_9P (deferred to its own chunk) | Unblocked — fd surface is now sufficient |
| SYS_MOUNT / SYS_UNMOUNT (deferred) | Unblocked |

---

## Known caveats / footguns

### Rights model is conservative

`SYS_DUP` requires `new_rights` to be a subset of `oldfd`'s rights. There's no way to elevate. For most use cases that's correct (POSIX dup preserves rights; the F_SET-FLAGS surface is separate). Future iterations may add a separate `sys_handle_reduce` for explicit rights-tightening on the original handle.

### No `dup2(oldfd, newfd)` variant

POSIX has both `dup(oldfd) → smallest free fd` and `dup2(oldfd, newfd) → forced specific fd`. v1.0 only ships `dup(oldfd)`. `dup2` requires a "force-close at target fd + install at exact slot" operation that the v1.0 handle table doesn't expose. Phase 5+ can add it.

### Per-call cap on SYS_READ / SYS_WRITE = 4 KiB

Userspace must loop for larger transfers. Documented in `53-sys-rw.md`.

### /pipe-probe size = 16 KiB

The libt linker script + inline syscall stubs push pipe-probe over the 16 KiB ceiling used by the existing virtio-* probes. The kernel test driver's static buffer is 32 KiB to accommodate. If many more probes accumulate, consolidate the static buffer or switch to heap allocation.

---

## Naming rationale

`SYS_CLOSE` = 11, `SYS_DUP` = 12 — append to the existing enumeration. Names match POSIX for familiarity. Internal names mirror the project convention.

`pipe-probe` matches the existing probe naming (`mmio-probe`, `irq-probe`, `virtio-blk-probe`, ...).

---

## Reference

- ARCH §11.2 (Core syscalls); §11.3 (Handle syscalls).
- `docs/reference/52-sys-pipe.md` (SYS_PIPE; KOBJ_SPOOR release/acquire wiring).
- `docs/reference/53-sys-rw.md` (SYS_READ / SYS_WRITE + uaccess_store_u8).
- `docs/reference/51-pipe.md` (the underlying pipe primitive).
- `docs/reference/19-handles.md` (handle table; HandleDup spec mapping).
- `specs/handles.tla` (RightsCeiling invariant pinning the elevation rejection).
