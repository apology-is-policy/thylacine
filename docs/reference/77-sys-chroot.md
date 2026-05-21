# 77 — `SYS_CHROOT` (P5-stratumd-stub-bringup-e2)

The v1.0 territory-root pivot syscall. Stamps the calling Proc's Territory `root_spoor` to the given `KOBJ_SPOOR`, so a subsequent `SYS_WALK_OPEN(spoor_fd == -1, ...)` walks from the pivoted root instead of an explicit handle.

Per `CORVUS-DESIGN.md §10.1` (`"pivot_root (or chroot at v1.0; full pivot at v1.x)"`) + `ARCHITECTURE.md §11.2` (syscall row). Spec: `specs/territory.tla::Chroot(p, s)`. Audit-trigger surface: `kernel/territory.c` (Territory) + `kernel/syscall.c` (syscall entry).

---

## Purpose

After `SYS_ATTACH_9P` + `SYS_MOUNT` give a Proc a `KOBJ_SPOOR` rooted at some 9P session's tree, the question is how the Proc reaches files under that tree. v1.0 has no multi-component path resolver yet — `SYS_WALK_OPEN` is single-component. The v1.0 minimum: a Proc can pivot its territory's root to point at the mounted Spoor, then walk single components from there via `SYS_WALK_OPEN(-1, "name", ...)`.

This is the v1.0 chroot mechanism per CORVUS-DESIGN.md `§10.1 Q2` — simpler than `pivot_root` (which v1.x will adopt), but sufficient for the boot path (joey-orchestrated stratumd integration) once the production walker lands. Until then, `SYS_WALK_OPEN` is the only consumer of `root_spoor`.

---

## Syscall ABI

```c
// kernel/include/thylacine/syscall.h
SYS_CHROOT = 35
```

| Register | Meaning |
|---|---|
| `x0` | `spoor_fd` (signed) — `hidx_t` of a `KOBJ_SPOOR` handle in the caller's handle table |
| `x8` | syscall number = 35 |
| `x0` (return) | `0` on success; `-1` on any rejection |

### Failure modes

| Cause | Mechanism |
|---|---|
| Caller has no Territory | `t->proc->territory == NULL` (kernel invariant; structurally impossible for a userspace Proc; defense-in-depth) |
| `spoor_fd` not a valid handle | `sys_lookup_spoor` returns NULL |
| Handle is not `KOBJ_SPOOR` | `sys_lookup_spoor`'s kind check |
| Handle lacks `RIGHT_READ` | `sys_lookup_spoor`'s rights gate |
| `territory_chroot` returned -1 | source NULL (already caught above; pure defense-in-depth) |

### Idempotency

`SYS_CHROOT(fd)` where the underlying Spoor is the same as the Territory's existing `root_spoor` is a `0` no-op. The Territory's refcount on that Spoor is unchanged.

---

## libt wrapper

```c
// usr/lib/libt/include/thyla/syscall.h
long t_chroot(long spoor_fd);
// 0 / -1
```

Consumed by `/stub-walk-probe` (the kernel-test probe) at P5-stratumd-stub-bringup-e2. Production callers (a future `init`-equivalent that wants `/sysroot` as `/`) will use the same wrapper.

`T_WALK_OPEN_FROM_ROOT == (long)-1` — pass as `spoor_fd` to `t_walk_open` to walk from the pivoted root.

---

## Kernel implementation

### Handler: `sys_chroot_handler` (`kernel/syscall.c`)

Thin SVC wrapper over `territory_chroot`:

1. Resolve `t = current_thread()`; check non-NULL.
2. `p = t->proc`; check non-NULL; check `p->territory` non-NULL.
3. `sys_lookup_spoor(p, (hidx_t)spoor_fd_raw, RIGHT_READ)` — KOBJ_SPOOR + RIGHT_READ gate.
4. `territory_chroot(p->territory, source)` — does the actual ref discipline.

The handler holds no locks across the call — `territory_chroot` is the source of truth for refcount discipline.

### Mechanism: `territory_chroot` (`kernel/territory.c`)

```c
int territory_chroot(struct Territory *territory, struct Spoor *source) {
    // ... NULL-checks omitted ...
    if (territory->root_spoor == source) return 0;        // idempotent
    struct Spoor *old = territory->root_spoor;
    spoor_ref(source);                  // bump BEFORE swap
    territory->root_spoor = source;
    if (old) spoor_clunk(old);          // MREPL-style displacement
    return 0;
}
```

Three load-bearing properties:

1. **Bump-before-swap**: `spoor_ref(source)` runs first. If `source` is corrupted (magic mismatch), `spoor_ref` extincts — `root_spoor` is left unchanged.
2. **`spoor_clunk` not `spoor_unref` on displaced root**: if a prior root was set and this is its last holder, the Dev's `close` hook fires (tears down per-Spoor Dev state). Same discipline as `mount()`'s MREPL displacement.
3. **Idempotent same-pointer**: a no-op `0`; no refcount bump. Mirrors `mount()`'s `(target, source) already in table → 0` semantics.

### Territory lifecycle integration

- **`territory_init_fields`**: `root_spoor = NULL` on fresh allocation.
- **`territory_clone`** (rfork path): if `parent->root_spoor != NULL`, `spoor_ref(parent->root_spoor)` then assign to `child->root_spoor`. Each cloned Territory contributes one new reference (matches the spec's `ForkClone` action's `+ (IF root_spoor[parent] = s THEN 1 ELSE 0)` refcount update).
- **`territory_unref` final-release**: if `root_spoor != NULL`, `spoor_clunk(root_spoor)` BEFORE `kmem_cache_free`. Mirrors the mount-entry drop loop above it.

---

## Spec

`specs/territory.tla::Chroot(p, s)`:

```tla
Chroot(p, s) ==
    /\ root_spoor[p] # s
    /\ root_spoor' = [root_spoor EXCEPT ![p] = s]
    /\ refcount' = IF root_spoor[p] = NONE
                   THEN [refcount EXCEPT ![s] = @ + 1]
                   ELSE [refcount EXCEPT ![s] = @ + 1,
                                        ![root_spoor[p]] = @ - 1]
    /\ UNCHANGED <<bindings, mounts>>
```

The chunk-extended invariant `MountRefcountConsistency`:

```tla
MountRefcountConsistency ==
    \A s \in Spoors :
        refcount[s] = Cardinality(MountEntriesForSpoor(s))
                    + Cardinality({p \in Procs : root_spoor[p] = s})
```

Buggy cfg `territory_buggy_chroot_no_refbump.cfg` (BUGGY_CHROOT_NO_REFBUMP=TRUE) produces a counterexample at depth 2 / 205 states: BuggyChrootNoRefbump stamps `root_spoor[p] = s` but skips the ref bump, so `refcount[s] = 0` while the kernel's actual contribution is 1 → MountRefcountConsistency violated.

---

## SYS_WALK_OPEN's FROM_ROOT path (companion change)

`SYS_WALK_OPEN` was extended at this chunk to accept the `SYS_WALK_OPEN_FROM_ROOT = (u64)-1` sentinel as its `spoor_fd` argument. When set, the handler resolves the source Spoor from `current_proc->territory->root_spoor` instead of looking up a handle:

```c
if (spoor_fd_raw == SYS_WALK_OPEN_FROM_ROOT) {
    if (!p->territory) return -1;
    src = p->territory->root_spoor;
    if (!src) return -1;
} else {
    src = sys_lookup_spoor(p, (hidx_t)spoor_fd_raw, RIGHT_READ);
    if (!src) return -1;
}
```

The rest of the handler (clone + walk + open + handle_alloc) is identical. The FROM_ROOT path does NOT `spoor_ref` the source itself — the Territory's existing reference keeps the Spoor alive across the syscall.

---

## Tests

### Kernel-internal (`kernel/test/test_territory_chroot.c`)

| Test | Coverage |
|---|---|
| `territory.chroot_smoke` | smoke: chroot bumps ref 1→2, territory_unref drops back to 1 |
| `territory.chroot_idempotent_same_spoor` | re-chroot to same Spoor returns 0, no second ref bump |
| `territory.chroot_replace_clunks_old` | chroot s1 → chroot s2: s1's ref dropped, s2's bumped |
| `territory.chroot_clone_bumps_ref` | territory_clone bumps root_spoor ref; destroy each → drops 1 each |
| `territory.chroot_destroy_drops_ref` | territory_unref's final-release drops root_spoor ref |
| `territory.chroot_null_returns_error` | territory_chroot(p, NULL) returns -1; no state change |

### End-to-end via `/stub-walk-probe`

`kernel/test/test_stratumd_stub.c::test_stratumd_stub_walk_round_trip` wires `stratumd-stub` + `stub-walk-probe` over 2 pipe pairs. The probe (a child Proc that exits naturally — releasing the chroot via `territory_unref`) executes:

1. `t_attach_9p(0, 1, "/", 1, 0)` → `attach_fd`
2. e1 path: `t_walk_open(attach_fd, "hello", 5, T_OREAD)` → walk + read + EOF + close
3. **e2 path**: `t_chroot(attach_fd)` → 0
4. **e2 path**: `t_walk_open(T_WALK_OPEN_FROM_ROOT, "hello", 5, T_OREAD)` → walk + read + EOF + close
5. `t_close(attach_fd)`; `t_exits(0)`

The probe's exit releases the territory's `root_spoor` ref → the attach Spoor's last-ref drop → adapter teardown → transport-Spoor EOF → stratumd-stub sees EOF on `c2s_rd` → exits 0 → kernel test reaps both Procs at status 0 → test PASS.

---

## Joey deliberately does NOT exercise SYS_CHROOT

joey is the long-running init Proc that never exits during boot. A `t_chroot` in joey's stub-bringup phase would stamp `joey->territory->root_spoor` with a ref on the attach Spoor, holding the underlying `p9_attached` (and its transport-Spoor refs) alive past joey's `t_close(attach_fd)` — stratumd-stub would never see EOF on `c2s_rd`, and joey's `t_wait_pid` would deadlock. Until v1.x lands `pivot_root` (or `chroot(NULL)`-clear semantics), the chroot path is exercised by short-lived child Procs (`stub-walk-probe`) and kernel-internal tests, NOT by joey. See `18-territory.md` "`chroot` is one-way at v1.0" for the full discipline note.

---

## Audit-bearing posture

Per `CLAUDE.md §25.4`:

| Surface touched | Why |
|---|---|
| `kernel/syscall.c` (entry point) | New syscall (capability checks: all syscall entry points) |
| `kernel/territory.c` (Territory) | Cycle-freedom (I-3), isolation (I-1), mount-refcount consistency (`§9.6.6` extended) — root_spoor adds to MountRefcountConsistency's formula |
| Handle table | `sys_lookup_spoor` (kind + rights checks); identical to other syscalls that consume KOBJ_SPOOR handles |

A focused audit round on SYS_CHROOT (bundled with the deferred SYS_WALK_OPEN focused round per the e1 close) is appropriate before promoting either syscall to a Phase-6+ multi-component walker or to a `pivot_root` v1.x extension.

---

## v1.x extensions

| Feature | Notes |
|---|---|
| `pivot_root(new_root, put_old)` | Plan-9-style: re-mount old root somewhere accessible before swapping. Per CORVUS-DESIGN.md §10.1 Q2 — chroot for v1.0, pivot for v1.x (frees the initrd RAM-resident pages). |
| `SYS_UNCHROOT` (or `SYS_CHROOT(NULL)`) | Clear root_spoor. Lets a long-running Proc relinquish a pivot. Needed once Proc-internal phases want to come and go. |
| Multi-component walker | The production `open(name, mode)` namec walker consumes mount entries + root_spoor in unified path resolution. `SYS_WALK_OPEN(FROM_ROOT, ...)` becomes a layer the walker composes on. |
