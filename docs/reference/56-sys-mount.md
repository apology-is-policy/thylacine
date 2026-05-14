# 56. SYS_MOUNT / SYS_UNMOUNT — userspace mount-table operations (P5-mount-syscall)

The user-visible body of ARCH §9.6.1's `mount(source_spoor_fd, target_path, flags) → 0` and `unmount(target_path)`. The last deferred SVC handler from Phase 5's plan; with this chunk all user-visible mount/attach syscalls from ARCH §11.2 are live.

This chunk also includes a structural fix to `kernel/spoor.c::spoor_clunk` — switching from "run dev->close on every clunk" to Plan 9 cclose semantics (run dev->close ONLY on the last drop). The prior behavior would have torn down the per-Spoor Dev state (pipe endpoints, 9P sessions) on the user's first close, even while a mount-table entry still held a reference. See §"Spoor lifecycle fix" below.

---

## Purpose

The Territory's mount table existed since P5-attach-mount (`kernel/territory.c::mount` / `::unmount` + `specs/territory.tla::MountRefcountConsistency`). SYS_MOUNT / SYS_UNMOUNT are the user-visible SVC entry points that wrap those C-API primitives.

Composition with the rest of Phase 5:

```
                ┌─────────────────────────────────────────────┐
                │   userspace                                 │
                └─────────────────────┬───────────────────────┘
                                       │
                ┌──── SYS_PIPE ────────┼────── SYS_ATTACH_9P ─┐
                │ (Spoor pair)         │  (dev9p Spoor)       │
                ▼                      ▼                      ▼
        ┌─────────────────────────────────────────────────────┐
        │  kernel/handle.c — KOBJ_SPOOR fds (caller's table)  │
        └─────────────────────────────────────────────────────┘
                                       │
                                       ▼
                            SYS_MOUNT(fd, path_id, flags)
                                       │
                                       ▼
        ┌─────────────────────────────────────────────────────┐
        │  kernel/territory.c — mount table (per Proc)        │
        │     mounts[path_id] = (Spoor *, flags)              │
        │     spoor_ref'd; lifetime bound to Territory        │
        └─────────────────────────────────────────────────────┘
```

The source Spoor can be ANY KOBJ_SPOOR — a dev9p-backed root from SYS_ATTACH_9P, a kernel-Dev-backed root (devramfs, devcons), a pipe end (legal but unusual — walking a pipe-as-mount mostly produces -1), or a future cross-territory share.

---

## ABI

### SYS_MOUNT

```
SYS_MOUNT = 14

Args:
  x0 = source_spoor_fd  — hidx_t; KOBJ_SPOOR handle in the caller's table
  x1 = target_path_id   — u32; abstract path token (see "Path IDs at v1.0" below)
  x2 = flags            — u32; MREPL / MBEFORE / MAFTER / MCREATE

Return:
  x0 = 0  on success;
  x0 = -1 on:
    - source_spoor_fd not a KOBJ_SPOOR or out-of-range;
    - missing RIGHT_READ on the source handle;
    - flags has bits outside MREPL|MBEFORE|MAFTER|MCREATE;
    - target_path_id or flags raw value above u32 max;
    - Territory mount table full (PGRP_MAX_MOUNTS = 8);
    - Proc has no Territory (defensive — kernel-internal-only path).
```

### SYS_UNMOUNT

```
SYS_UNMOUNT = 15

Args:
  x0 = target_path_id  — u32; the path to remove a mount from

Return:
  x0 = 0  if an entry was found + removed;
  x0 = -1 on:
    - no entry exists at target_path_id;
    - target_path_id raw value above u32 max;
    - Proc has no Territory.
```

### Path IDs at v1.0

`target_path_id` is a u32 abstract path token — the same identifier used by the existing `bind` / `unbind` / `mount` / `unmount` C-API (`path_id_t` in `kernel/include/thylacine/territory.h`). The Territory's mount table is keyed on these u32s today.

String-path resolution (walk through the namespace tree to produce a path_id) is a future chunk — when the fd-syscall walk subsystem lands, an additional SVC entry will copy a user-VA string and resolve it through bind/mount before reaching this layer. The handler signature stays the same; only its caller chain changes.

Userspace can pick its own conventions for path IDs at v1.0 (e.g., test programs use small integers like 42).

### Flags

`MREPL` / `MBEFORE` / `MAFTER` / `MCREATE` mirror Plan 9 (see `kernel/include/thylacine/territory.h`). At v1.0 only `MREPL` has distinguished semantics — when set and an entry at the same target_path_id exists, the existing entry is **replaced** (its source is `spoor_clunk`'d, the new source is `spoor_ref`'d). Without `MREPL`, a same-target entry results in an "append a new entry" (union mount; walk-side union support is Phase 5+).

Idempotency: mounting the same `(source_spoor_fd, target_path_id)` pair twice is a no-op success (matches `specs/territory.tla::Mount`'s `<<path, s>> \notin mounts[p]` precondition).

### Rights gates

`RIGHT_READ` on the source handle. Rationale: a mount holder consumes the source's tree (walks it, reads files through it). A WRITE-only handle is structurally useless as a mount source. This is a defense-in-depth check — the actual walk-side enforcement comes from the Dev's read hook checking its own state.

`RIGHT_TRANSFER` is NOT required at v1.0 — mount is a Territory-local operation, not a cross-Proc transfer. When cross-Territory mount sharing (RFNAMEG=1) gets a richer Phase 5+ semantic, transfer rights will gate it.

---

## Userspace API — `<thyla/syscall.h>`

```c
__attribute__((always_inline))
static inline long t_mount(long source_spoor_fd, unsigned long target_path_id,
                           unsigned long flags);

__attribute__((always_inline))
static inline long t_unmount(unsigned long target_path_id);

#define T_MREPL    0x0001u
#define T_MBEFORE  0x0002u
#define T_MAFTER   0x0004u
#define T_MCREATE  0x0008u
```

Direct mirrors of the kernel ABI. Returns 0 / -1.

---

## Implementation

`kernel/syscall.c` exposes two pairs:

- `sys_mount_for_proc(p, source_fd, target, flags)` — non-static inner; kernel-internally testable. Validates flags + RIGHT_READ + Territory presence; routes into `kernel/territory.c::mount`.
- `sys_mount_handler(...)` — static SVC wrapper. Validates u32 bounds on the raw u64 args; pulls `current_thread()->proc`; calls the inner.
- `sys_unmount_for_proc(p, target)` + `sys_unmount_handler(...)` — symmetric pair for unmount.

The handler is thin because the heavy lifting (idempotency, MREPL semantics, refcount discipline, table-full handling, MountRefcountConsistency invariant) lives in `kernel/territory.c::mount` / `::unmount` and is pinned by `specs/territory.tla`.

### Lifecycle (mount → close → unmount)

Per ARCH §9.6.6 + the Plan 9 cclose fix in this chunk:

1. User creates a transport — `t_pipe(&fd_rd, &fd_wr)` or `t_attach_9p(...)`.
2. User mounts the source — `t_mount(fd_rd, 42, 0)`. Internally:
   - `sys_lookup_spoor` validates KOBJ_SPOOR + RIGHT_READ.
   - `territory.c::mount` is called.
   - `spoor_ref(source)` — mount entry now holds its own ref.
   - Entry installed at `target=42`.
3. User can `t_close(fd_rd)`. Internally:
   - `handle_release_obj` → `spoor_clunk(rd_spoor)`.
   - Plan 9 cclose: ref drops from 2 → 1 (mount entry's ref keeps it alive). `dev->close` NOT called.
   - The pipe endpoint + ring + Spoor all stay alive.
4. Later, `t_unmount(42)`. Internally:
   - `territory.c::unmount` finds the entry, removes it.
   - `spoor_clunk(source)` — last drop. `dev->close` (devpipe_close) runs: sets EOF, drops ring ref, frees endpoint. `spoor_free_internal` finalizes the Spoor.

If the user never `t_unmount`s, Territory destruction (last Proc using this Territory exits) does the cleanup through `territory_unref`'s final-release loop, which now uses `spoor_clunk` (was `spoor_unref`) so per-Spoor Dev state is released.

### Composition with SYS_ATTACH_9P

A dev9p-backed root Spoor from SYS_ATTACH_9P carries a populated `attached_owner` field. When that Spoor's last ref drops (e.g., user closed their attach_9p fd + a later unmount drops the mount-table's ref), dev9p_close runs:
- `p9_attached_destroy` → `p9_client_destroy` + `recv_buf` free + `p9_attached` struct free.
- `spoor_clunk` (not the prior `spoor_unref`) on the transport Spoors — propagates close down the stack so the transport's own Dev close runs on its last ref.
- `kfree` the kmalloc'd adapter.

This means: mount a dev9p root, close the attach_9p fd, do work through the mount, eventually unmount — and the entire 9P session is torn down at the unmount point (or Territory destruction).

---

## Spoor lifecycle fix (P5-mount-syscall companion)

Refactoring `kernel/spoor.c::spoor_clunk` to Plan 9 cclose semantics is included in this chunk because P5-mount-syscall is the first feature where the prior behavior was actively wrong.

### Before

```c
void spoor_clunk(struct Spoor *c) {
    ...
    if (c->dev && c->dev->close) c->dev->close(c);  // ← always called
    spoor_unref(c);   // ref--; if 0, free
}
```

Every clunk ran `dev->close` regardless of whether other refs existed. The doc comment claimed "close hooks are responsible for being idempotent + safe-on-not-yet-opened" — but `devpipe_close` (and `dev9p_close` with `attached_owner`) are emphatically NOT idempotent. They unconditionally tear down per-Spoor Dev state on every invocation.

### After

```c
void spoor_clunk(struct Spoor *c) {
    ...
    if (c->ref == 1) {
        // Last drop. Run Dev close (with ref still 1 so the hook sees
        // a valid Spoor), then drop to 0 and free.
        if (c->dev && c->dev->close) c->dev->close(c);
        c->ref = 0;
        spoor_free_internal(c);
        return;
    }
    c->ref--;
}
```

`dev->close` runs strictly once, on the last holder's clunk. Extra refs (dup'd handles, mount-table entries) keep the Dev state alive until they too clunk. This matches Plan 9's `cclose` and is what ARCH §9.6.6's lifecycle requires.

### Companion site changes

`kernel/territory.c` switched three `spoor_unref` call sites to `spoor_clunk`:

1. `mount()`'s MREPL path — when displacing an existing mount-table entry, the displaced source's holder is releasing it; if it was the last ref, Dev close should run.
2. `unmount()` — the per-entry refcount drop; same rationale.
3. `territory_unref()`'s final-release loop — when a Territory's last ref goes away, each remaining mount entry is implicitly unmounted; same rationale.

`kernel/dev9p.c::dev9p_close` switched two `spoor_unref` calls to `spoor_clunk` — the transport Spoors that the SYS_ATTACH_9P handler took independent refs on. If userspace closed their transport fds before the attach root was closed, the dev9p-side refs are the last holders; switching to `spoor_clunk` makes the pipe endpoints (or other transport Dev state) tear down cleanly on that last release.

### Sites kept on `spoor_unref`

- `kernel/dev9p.c::dev9p_attach_client` constructor failure path — Spoor was just `spoor_alloc`'d, ref=1, no aux installed; freeing is correct without close (dev9p_close would no-op on the un-populated priv anyway).
- `kernel/syscall.c::sys_attach_9p_handler` error rollback paths — each `spoor_unref` balances an earlier `spoor_ref` taken in the same handler. The user's fd still holds its own ref; this is a paired refcount cancel, not a last-drop.

### Why this is a P5-mount-syscall fix and not a separate chunk

The prior `spoor_clunk` semantics worked for every Phase 5 test up to and including SYS_ATTACH_9P because every Spoor in those scenarios had exactly ONE holder (the user's handle table). The asymmetry only becomes a bug when a second holder exists — and SYS_MOUNT is the first user-visible feature that creates one. Bundling the fix with the chunk that exposes it keeps the spec ↔ code ↔ behavior alignment legible in `git log` rather than splitting a "see prior commit" detective trail across chunks.

---

## Data structures

No new types. The handler operates on existing `path_id_t` / `struct PgrpMount` / `struct Territory` from `kernel/include/thylacine/territory.h` (pinned at 216 bytes by `_Static_assert` at P5-attach-mount).

---

## State machines

None per se — the handler is a stateless RPC over the Territory's mount table. The state machine is the Territory's mount table itself, modeled by `specs/territory.tla` (P5-attach-mount).

---

## Spec cross-reference

The SVC handlers are user-visible wrappers over `kernel/territory.c::mount` and `::unmount`, which materialize the spec's `Mount` and `Unmount` actions:

| Spec action | Code |
|---|---|
| `Mount(p, s, path, flags)` | `kernel/syscall.c::sys_mount_for_proc` → `kernel/territory.c::mount` |
| `Unmount(p, path)` | `kernel/syscall.c::sys_unmount_for_proc` → `kernel/territory.c::unmount` |
| `MountRefcountConsistency` | `_for_proc` inners + the `kernel/territory.c` body together preserve it; no new spec needed |

The Plan 9 cclose semantics fix is invisible at the spec level — the spec models refcount drops abstractly; whether the drop runs a Dev close hook on last is an implementation detail.

---

## Tests

`kernel/test/test_sys_mount.c` — 9 tests:

- `sys_mount.happy_path_grafts_pipe_spoor` — pipe-Spoor source; mount at path_id=42; nmounts 0→1.
- `sys_mount.idempotent_on_duplicate` — same `(source, target)` twice; nmounts stays 1.
- `sys_mount.rejects_bad_fd` — out-of-range / negative / closed fds → -1.
- `sys_mount.rejects_missing_right_read` — `handle_dup` with `RIGHT_WRITE`-only; mount returns -1.
- `sys_mount.rejects_invalid_flags` — bits outside `MREPL|MBEFORE|MAFTER|MCREATE` → -1; `MREPL` accepted.
- `sys_mount.rejects_null_territory` — Proc with no Territory → -1.
- `sys_unmount.removes_entry_and_drops_ref` — full mount → close fds → unmount → ring freed at unmount (proves the mount-table held the last live ref).
- `sys_unmount.rejects_nonexistent_target` — unmount of unmounted path / unrelated path → -1.
- `sys_mount.caller_close_keeps_mount_alive` — mount → close source fd → close other pipe end → ring STILL alive → Territory drop → ring freed. The end-to-end lifecycle test that validates the Plan 9 cclose fix.

Coverage NOT in this chunk:

- Userspace probe (similar to `/pipe-probe`) exercising `t_pipe` → `t_mount` → `t_unmount` end-to-end at EL0. Deferred to a later chunk (it would mostly mirror the kernel-internal tests through one extra layer).
- The cross-Proc mount-sharing test (RFNAMEG=1) — gated on the rfork-RFNAMEG path landing.

---

## Error paths

Every error is `-1` (Plan 9 convention; per-thread errstr lands later). Triggers:

- `current_thread() == NULL` → `-1` (defensive — handler entry guard; in practice always non-NULL after `proc_init`).
- `proc->territory == NULL` → `-1` (defensive — every Proc post-`rfork` has a Territory; kernel-internal call paths might not).
- `flags & ~(MREPL|MBEFORE|MAFTER|MCREATE)` → `-1` (caller passed bogus bits).
- `target_path_id > 0xFFFFFFFFu` or `flags > 0xFFFFFFFFu` (raw u64 inputs) → `-1` (path_id_t / flags must round-trip through u32).
- `handle_get(p, source_fd) == NULL` → `-1` (out-of-range, not present).
- `slot->kind != KOBJ_SPOOR` → `-1` (sys_lookup_spoor structural check).
- `(slot->rights & RIGHT_READ) == 0` → `-1` (rights gate).
- `kernel/territory.c::mount` returns nonzero (table full at PGRP_MAX_MOUNTS=8) → `-1`.
- `kernel/territory.c::unmount` returns -1 (no entry at target) → `-1`.

---

## Performance characteristics

Both handlers are O(N) in the Territory's mount-table size (`PGRP_MAX_MOUNTS = 8`). At v1.0 with N capped at 8, the entire operation is dozens of cycles plus the SVC entry/exit overhead. No locks, no allocations, no syscalls fanned out.

Not on any latency-sensitive critical path. VISION's 500 ms boot target is unaffected; the syscall itself completes in microseconds.

---

## Status

| Item | State |
|---|---|
| SVC handlers (mount + unmount) | LANDED |
| libt stubs (t_mount + t_unmount) | LANDED |
| Kernel-internal tests | LANDED (9 tests) |
| Plan 9 cclose semantics fix | LANDED (companion) |
| Userspace probe binary | Deferred |
| Path-string resolution | Deferred (needs walk subsystem) |
| Cross-Territory share (RFNAMEG=1) | Deferred (needs RFNAMEG path) |
| `specs/territory.tla` | Unchanged (composition over existing Mount / Unmount actions) |

Landed at commit (P5-mount-syscall substantive). Hash fixup follows.

---

## Known caveats / footguns

1. **Path IDs are abstract u32 tokens at v1.0.** Userspace agreeing on `42 = /stratum/data` is convention, not enforcement. The walk subsystem (later chunk) will replace this with kernel-resolved tokens.

2. **MREPL displaces silently.** A `mount(source, target, MREPL)` overwrites an existing entry at `target` and `spoor_clunk`s the prior source — if that was the last holder, the old Dev's close hook runs (which for dev9p means the entire 9P session tears down). The caller is responsible for ensuring nothing else needs the displaced source.

3. **No partial walk through mount.** This chunk wires the mount-table entry but does NOT teach the walk path to consult mounts[]. Walking through a mount point still uses the Plan 9 bind table (already implemented) — Phase 5+ extends walk to consider mount entries for full namespace dispatch.

4. **No string-path errors.** Bad `target_path_id` (out of u32 range) collapses to `-1`. When string resolution lands, ENOENT / EACCES / etc. will get richer errors via errstr.

5. **Plan 9 cclose semantics are NEW.** Existing test_pipe.c / test_sys_pipe.c tests pass under the new semantics because they each used Spoors with ref=1 (no dup, no mount). Future test patterns that intentionally test "close one of N holders" should now expect the underlying Dev state to STAY ALIVE — the prior eager-close behavior is gone.
