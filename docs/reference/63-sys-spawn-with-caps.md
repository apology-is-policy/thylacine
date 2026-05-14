# 63. SYS_SPAWN_WITH_CAPS — cap-subset spawning (P5-spawn-caps)

The third spawn variant. `SYS_SPAWN` gives the child zero caps; `SYS_SPAWN_WITH_FDS` gives the child specific Spoor fds with zero caps; this syscall gives the child `parent->caps & cap_mask`. Wraps the existing kernel-internal `rfork_with_caps` primitive (P4-Ic3). Needed at P5-corvus-bringup so joey can spawn `/sbin/corvus` with `CAP_LOCK_PAGES | CAP_CSPRNG_READ` — a subset of joey's `CAP_ALL`.

---

## Why a third spawn variant

By chunk:
- `SYS_SPAWN` (= 21, P5-spawn-wait) — minimum spawn; child gets zero caps + no fds.
- `SYS_SPAWN_WITH_FDS` (= 23, P5-stratumd-stub-b) — child gets specific fds + zero caps.
- `SYS_SPAWN_WITH_CAPS` (= 24, this) — child gets specific caps + no fds.
- Future `SYS_SPAWN_FULL` — child gets both fds and caps. Lands when a use case needs the combination (likely P5-corvus-bringup: corvus is spawned with both pipe ends from joey AND specific caps).

Three variants instead of one combined-everything syscall keeps each one's intent explicit at the call site. The cost (more entries in the switch) is small; the benefit (no ABI churn when adding capabilities — and clear opt-in semantics for each new capability) is meaningful.

---

## ABI

```
SYS_SPAWN_WITH_CAPS(name_va, name_len, cap_mask) → child_pid (>0) / -1

Args:
  x0 = name_va        user-VA pointer to the binary name
  x1 = name_len       bytes; 1..SYS_SPAWN_NAME_MAX = 64
  x2 = cap_mask       caps_t bitmask; child gets parent->caps & mask

Return:
  x0 = child_pid      >0 on success
  x0 = -1             on: same conditions as SYS_SPAWN
                      (name validation / binary lookup / blob / OOM)
```

---

## Cap arithmetic — monotonic-reduction structural enforcement

`sys_spawn_with_caps_for_proc` delegates to `rfork_with_caps`. Per P4-Ic3, that primitive enforces:

```c
caps_t parent_caps = __atomic_load_n(&parent->caps, __ATOMIC_ACQUIRE);
child->caps = parent_caps & caps_mask;
```

This is the **structural** enforcement of ARCH I-2 (capability set monotonically reduces) and ARCH I-6 (handle rights monotonically reduce on transfer). Even if the caller passes a `cap_mask` with bits the parent doesn't hold, the bitwise AND clamps to `parent->caps`. The extra bits are silently dropped — no rejection needed. The acquire fence on the parent's caps read prevents torn-intermediate-state hazards from a concurrent `ReduceCaps` on another CPU.

Important: this is enforced on the wrapper side too. `SYS_SPAWN_WITH_CAPS`'s handler can't bypass the AND — there's no path that sets `child->caps` outside `rfork_with_caps`. A malicious userspace can pass `cap_mask = 0xFFFFFFFFFFFFFFFFull` and won't escalate beyond what they already had.

---

## Userspace API — `<thyla/syscall.h>`

```c
long t_spawn_with_caps(const char *name, size_t name_len, unsigned long cap_mask);
```

Inline-asm stub matching the kernel ABI. The cap bit values mirror `kernel/include/thylacine/caps.h`:

```c
#define CAP_HW_CREATE   (1u << 0)  // hardware resource creation
#define CAP_LOCK_PAGES  (1u << 1)  // SYS_MLOCKALL
#define CAP_CSPRNG_READ (1u << 2)  // SYS_GETRANDOM
// future: CAP_HOSTOWNER (lands at P5-hostowner)
```

---

## Implementation

The handler is a thin wrapper:

1. Validates name length + user-VA buffer + per-byte copy (same as SYS_SPAWN).
2. Calls `sys_spawn_with_caps_for_proc(p, name, name_len, cap_mask)`.
3. The inner does devramfs_lookup + 8-aligned blob copy + kmalloc args + `rfork_with_caps(RFPROC, sys_spawn_thunk, args, cap_mask)`.
4. Reuses the existing `sys_spawn_thunk` (from SYS_SPAWN) — the only difference between SYS_SPAWN and SYS_SPAWN_WITH_CAPS is the rfork variant called.

No new state; no new lifetime concerns; the cap arithmetic is delegated entirely to `rfork_with_caps`.

---

## Tests

`kernel/test/test_sys_spawn_with_caps.c` — 5 tests covering the syscall wrapper semantics:

- `sys_spawn_with_caps.happy_path_zero_mask` — `cap_mask=CAP_NONE` → child gets zero caps + exits cleanly. Verifies via `proc_find_by_pid` that `child->caps == 0` before reaping.
- `sys_spawn_with_caps.happy_path_subset_of_parent` — `cap_mask=CAP_LOCK_PAGES` → child gets exactly `CAP_LOCK_PAGES` (since kproc carries `CAP_ALL`).
- `sys_spawn_with_caps.clamps_to_parent` — `cap_mask=0xFFF...FF` → child clamped to `CAP_ALL`; no spurious bits. The I-2 / I-6 monotonic-reduction is enforced structurally.
- `sys_spawn_with_caps.rejects_missing_binary` — name = `nonexistent` → -1.
- `sys_spawn_with_caps.rejects_oversize_name` — `name_len > SYS_SPAWN_NAME_MAX` → -1.

The cap inspection pattern is **read-then-reap**: tests find the child via `proc_find_by_pid` and read `child->caps` BEFORE calling `wait_pid`. caps are stable for a Proc's lifetime (only `ReduceCaps` modifies them, and the spawned child doesn't call it), so the read is race-free even if the child runs concurrently on another CPU.

Test count: 430 → 435 PASS × default + UBSan.

---

## Why not check the cap arithmetic from userspace

There's no `t_getcaps`-style syscall at v1.0 — a Proc can't introspect its own caps from userspace. The cap arithmetic is verified at the kernel level via `proc_find_by_pid`, which is sufficient for regression coverage. A future userspace `getcaps()` (when corvus needs to assert its caps were applied) lands as a separate small chunk.

---

## Composition with future chunks

- **P5-corvus-bringup**: joey will call `t_spawn_with_caps("corvus", 6, T_CAP_LOCK_PAGES | T_CAP_CSPRNG_READ)`. Corvus's startup then calls `t_mlockall(0)` + `t_set_dumpable(0)` + `t_set_traceable(0)` + `t_getrandom(...)` — all gated on caps it just received.
- **P5-spawn-full** (future): combined `SYS_SPAWN_FULL(name, len, fds, fd_count, cap_mask)`. Needed when joey spawns corvus with both a pre-opened `/srv/corvus/` Spoor pair AND CAP_LOCK_PAGES + CAP_CSPRNG_READ. Probably lands at P5-corvus-bringup.
- **P5-hostowner**: introduces `CAP_HOSTOWNER`. `CAP_ALL` extends; `_Static_assert` in caps.h catches drift. SYS_SPAWN_WITH_CAPS continues to work unchanged.

---

## Status

| Item | State |
|---|---|
| `SYS_SPAWN_WITH_CAPS` handler + `sys_spawn_with_caps_for_proc` inner | LANDED |
| libt `t_spawn_with_caps` stub | LANDED |
| Cap arithmetic verified via `proc_find_by_pid` inspection | LANDED |
| 5 kernel-internal tests (3 happy paths + 2 rejection) | LANDED |
| `SYS_SPAWN_FULL` (combined fds + caps) | DEFERRED (lands at P5-corvus-bringup or earlier when needed) |
| `getcaps()` userspace introspection | DEFERRED (needed when a Proc must assert its caps) |

---

## Known caveats

1. **No userspace introspection.** A spawned child can't query its own caps from userspace at v1.0. Workaround: a syscall that requires a cap fails with -1 if the cap is missing; the caller infers. A dedicated `getcaps` lands when needed.
2. **No combined-fds-and-caps yet.** corvus startup eventually needs `SYS_SPAWN_FULL` (both inherited fds + cap-subset). Held until that chunk; no v1.0 use case currently needs both.
3. **`CAP_NONE = 0` is implicit.** Passing `cap_mask=0` gives the child zero caps — equivalent to `SYS_SPAWN`. The two syscalls coexist; `SYS_SPAWN` is preferred for clarity when no caps are needed.
4. **No syscall-level mask validation.** Any 64-bit value in `cap_mask` is accepted; bits outside `CAP_ALL` are silently dropped at the AND. This matches the parent's `caps_t` semantics — `caps_t` is a bitmask, and undefined bits have no meaning. If a future invariant requires "reject masks with undefined bits set," it lands as a wrapper check.
