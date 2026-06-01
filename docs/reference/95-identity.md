# 95 — Identity model (A-1a)

**Status:** A-1a landed (kernel half). principal-id / groups on the Proc, inheritance
across rfork/spawn, identity-at-spawn gated by `CAP_SET_IDENTITY`, and the
`srv_peer_info` identity exposure. A-1b (corvus identity DB + `RESOLVE_*` + CRVS v2)
is the userspace authority half and is not yet landed.

Design: `docs/IDENTITY-DESIGN.md` (§1 five-axis model, §3.3 hybrid identity, §3.7
kernel-VFS permission enforcement, §9.1 the A-1 ABI pin). Invariant: ARCH §28 I-22.

---

## Purpose

Thylacine's identity axis (one of the five — visibility / **identity** / permission /
capability / confidentiality). A Proc carries a **durable identity** — an opaque
`principal_id` plus a primary group and a small cache of supplementary groups — that
is the stable answer to "*who* is this process running as." It is deliberately
orthogonal to authority: identity confers **no** capabilities (I-22). Authority lives
entirely on the capability axis (`caps` + handle rights); the durable identity exists
to attribute ownership (files created, audit) and to drive the permission axis (the
A-2d kernel rwx check, future), never to grant kernel power.

Three per-Proc credential-ish fields, each with a different lifecycle:

| Field | Lifecycle | Set by |
|---|---|---|
| `principal_id` + groups | **durable**, inherited unchanged across rfork/spawn | `proc_init` (kproc=SYSTEM); `rfork_internal` (inherit); the spawn thunk (override) |
| `caps` | monotonically **reduces** (rfork mask-AND) | `rfork_internal` (`parent->caps & mask`) |
| `stripes` | **fresh** per Proc, immutable | `proc_alloc` (monotonic counter) |

corvus is the authority for the `id <-> name <-> groups <-> keys` mapping (A-1b); the
kernel only stores + inherits + exposes the active set.

---

## Public API

### Reserved values (`<thylacine/proc.h>`)

```c
#define PRINCIPAL_INVALID  0u           // never assigned; KP_ZERO default, fail-closed
#define PRINCIPAL_SYSTEM   0xFFFFFFFEu  // boot/kernel-proc identity (kproc, joey, pre-login)
#define PRINCIPAL_NONE     0xFFFFFFFFu  // unauthenticated "nobody" (Plan 9 `none`); dead-peer
#define GID_INVALID        0u
#define GID_SYSTEM         0xFFFFFFFEu
#define GID_NONE           0xFFFFFFFFu
#define PROC_SUPP_GIDS_MAX 15           // 1 primary + up to 15 supplementary = 16 total
```

None of these is privileged (I-22): `SYSTEM` holds caps via the boot chain, not via
its identity. Real users/roles are corvus-assigned in `[1, 0xFFFFFFFD]`.

### Proc fields (`struct Proc`)

```c
u32 principal_id;                  // durable identity (inherited)
u32 primary_gid;
u32 supp_gids[PROC_SUPP_GIDS_MAX]; // [0..supp_gid_count) live; tail zeroed
u8  supp_gid_count;
```

### Functions (`<thylacine/proc.h>`)

```c
// The single audited identity-mutation site. Sets principal_id/primary_gid and
// copies the first supp_gid_count gids (zeroing the tail). Extincts on a reserved
// principal_id/primary_gid (INVALID/SYSTEM -- A-1a R1 F3) or a NULL/
// corrupted Proc or supp_gid_count > PROC_SUPP_GIDS_MAX. Confers NO caps (I-22).
void proc_apply_identity(struct Proc *p, u32 principal_id, u32 primary_gid,
                         const u32 *supp_gids, u8 supp_gid_count);

// One-walk snapshot (under g_proc_table_lock) of caps + principal_id +
// primary_gid for the ALIVE Proc carrying `stripes`. Out-params may be NULL.
// Fail-closed: stripes==0 or no ALIVE match -> false, out-params untouched.
bool proc_peer_snapshot_by_stripes(u64 stripes, caps_t *caps_out,
                                   u32 *principal_out, u32 *primary_gid_out);

// caps-only wrapper (unchanged API for existing SYS_SRV_PEER callers).
bool proc_caps_by_stripes(u64 stripes, caps_t *caps_out);
```

### Capability (`<thylacine/caps.h>`)

```c
#define CAP_SET_IDENTITY (1ull << 5)   // gates SPAWN_IDENTITY_SET; in CAP_ALL
```

`CAP_SET_IDENTITY` is the setuid-equivalent: a holder can mint a child running as any
user. FORK-GRANTABLE (member of `CAP_ALL`) so it flows kproc -> joey -> `/sbin/login`
down the vetted boot chain; an ordinary user Proc never holds it (login omits it from
the shell's `cap_mask`).

### Spawn ABI (`<thylacine/syscall.h>` — `struct sys_spawn_args`, 56 -> 80)

```c
u32 principal_id;   // @56  honored iff identity_flags & SPAWN_IDENTITY_SET
u32 primary_gid;    // @60
u64 supp_gids_va;   // @64  user-VA of supp_gid_count u32 gids
u32 supp_gid_count; // @72  0..15
u32 identity_flags; // @76  SPAWN_IDENTITY_SET = 1u<<0
```

### SYS_SRV_PEER ABI (`struct srv_peer_info`, 24 -> 40)

```c
u32 principal_id;   // @24  peer's durable identity; PRINCIPAL_NONE when alive == 0
u32 primary_gid;    // @28  peer's primary group; GID_NONE when alive == 0
u32 flags;          // @32  reserved 0
u32 _reserved;      // @36  reserved 0 (explicit pad to 40)
```

The fields append **after** the live `alive` field (@20) — the §9.1 pin originally
misread @20 as pad; corrected to 40 bytes at A-1a (see the commit + `IDENTITY-DESIGN`
§9.1 CORRECTION note).

---

## Implementation

### Inheritance (`kernel/proc.c::rfork_internal`)

After the caps reduce (`child->caps = (parent_caps & caps_mask) & ~CAP_ELEVATION_ONLY` —
the `~CAP_ELEVATION_ONLY` strip is A-4-pre, so no elevation-only cap such as
`CAP_HOSTOWNER` crosses a fork even from an elevated parent), the child copies the
parent's identity verbatim: `principal_id`, `primary_gid`, and the first
`supp_gid_count` supplementary gids. A plain read suffices — identity is never mutated
on a running Proc (the spawn override runs in the child's own thunk before
`userland_enter`), so the parent's identity is immutable for its life once set.

### kproc = SYSTEM (`kernel/proc.c::proc_init`)

kproc is stamped `{PRINCIPAL_SYSTEM, GID_SYSTEM}` alongside `caps = CAP_ALL`. Every
rfork descendant inherits SYSTEM until `/sbin/login` (A-5) stamps a real identity, so
the whole boot chain runs as SYSTEM. `proc_alloc`'s KP_ZERO leaves the fields at
`INVALID` (0) — the fail-closed transient before inherit/stamp runs.

### Identity-at-spawn (race-free)

`SYS_SPAWN_FULL_ARGV` -> `sys_spawn_full_argv_handler` reads the identity fields,
copies any supp gids, and routes through `sys_spawn_full_argv_identity_for_proc` (the
gate site), which:

1. Console-perm gate (existing) + the `CAP_SET_IDENTITY` gate (FAIL-CLOSED: a
   `SPAWN_IDENTITY_SET` request without the cap returns -1, never silent-inherit).
2. Reserved-value reject (`spawn_identity_value_ok`): `principal_id`/`primary_gid`
   must be `[1, 0xFFFFFFFD]` or `NONE`; `INVALID`/`SYSTEM` -> -1. `supp_gid_count <=
   15`; supplementary gids go through the **same predicate** (INVALID + SYSTEM both
   rejected -- A-1a R1 F1; no smuggling the system group into a supp set).
3. Passes the bundle into `spawn_full_argv_args` (the kmalloc'd thunk arg).

The thunk (`sys_spawn_full_argv_thunk`) runs in the **child** and, after the perm
stamps and before fd-install / exec, calls `proc_apply_identity` when
`identity.set` — so the child never enters EL0 under the wrong identity, and no second
post-spawn syscall is needed. The back-compat entry `sys_spawn_full_argv_for_proc`
delegates with `set_identity=false` (inherit), so existing callers and tests are
unchanged.

### SYS_SRV_PEER (`kernel/syscall.c::sys_srv_peer_for_proc`)

Resolves the peer fresh per query via `proc_peer_snapshot_by_stripes` and writes
`principal_id`/`primary_gid` (NONE when the peer is not alive), `flags`/`_reserved`=0.

---

## Userspace mirrors (ABI coordination)

The spawn-args and srv_peer_info structs are shared kernel <-> userspace; A-1a grew
all mirrors in lockstep:

| Mirror | File | Change |
|---|---|---|
| `struct t_sys_spawn_args` | `usr/lib/libt/include/thyla/syscall.h` | 56 -> 80 + `T_SPAWN_IDENTITY_SET` + `T_CAP_SET_IDENTITY` + `T_PRINCIPAL_*`/`T_GID_*` |
| `TSpawnArgs` | `usr/lib/libthyla-rs/src/lib.rs` | 56 -> 80 + `T_SPAWN_IDENTITY_SET` + `T_CAP_SET_IDENTITY` + sentinels |
| `TSrvPeerInfo` | `usr/lib/libthyla-rs/src/lib.rs` | 24 -> 40 |
| `struct pouch_srv_peer_info` | `usr/lib/pouch/patches/0006-pouch-sockets.patch` | 24 -> 40 (see Caveats) |

C callers use designated/whole-struct init, so the new fields auto-zero (inherit). The
Rust `TSpawnArgs` literal in `process.rs` sets them explicitly to 0 (inherit). The
pouch SO_PEERCRED `ucred.uid`/`gid` marshal still reports 0 — mapping principal-id to
the uid stratumd expects is an A-3 concern (it interacts with the boot-chain
SYSTEM-vs-0 assumption).

---

## Tests

`kernel/test/test_proc_identity.c` (8 tests; all run in kproc context):

- `kproc_is_system` — kproc = `{SYSTEM, GID_SYSTEM}`, supp_count 0.
- `rfork_inherits` — child inherits SYSTEM identity, caps reduce to NONE, stripes
  fresh. The single assertion set demonstrates I-22 (SYSTEM identity, zero authority).
- `apply_sets_fields` — `proc_apply_identity` sets fields + zeroes the supp tail.
- `spawn_set_rejected_without_cap` — an uncapped child's SET request -> -1.
- `spawn_set_accepted_with_cap` — a capped (kproc) SET spawn of `/hello` succeeds +
  exits clean.
- `set_rejects_reserved` — SET `INVALID`/`SYSTEM` principal, `SYSTEM` gid -> -1.
- `set_rejects_system_supp_gid` — SET supp gid `SYSTEM`/`INVALID` -> -1 (R1 F1).
- `peer_snapshot_by_stripes` — kproc resolves to SYSTEM; 0-sentinel + unassigned tag
  fail-closed.

Suite: 618/618 PASS (default + ASan + UBSan).

---

## Error paths

- `proc_apply_identity` — extinction on NULL/corrupted Proc, `supp_gid_count > 15`, or
  a `principal_id`/`primary_gid` that is the INVALID/SYSTEM sentinel (kernel-internal
  contract violation; the gate already bounds these -- R1 F3 makes the "single audited
  site" framing real).
- `sys_spawn_full_argv_identity_for_proc` — `-1` on: `SPAWN_IDENTITY_SET` without
  `CAP_SET_IDENTITY`; reserved/invalid id or gid; `supp_gid_count > 15`.
- `sys_spawn_full_argv_validate_req` — `-1` on unknown `identity_flags` bits, or
  `supp_gid_count > 15` when `SPAWN_IDENTITY_SET`.
- `proc_peer_snapshot_by_stripes` — `false` (out untouched) on `stripes==0` / no
  ALIVE match.

---

## Status

- **Implemented (A-1a):** Proc identity fields + inheritance + kproc=SYSTEM +
  `CAP_SET_IDENTITY` + identity-at-spawn (fail-closed gate, reserved-value reject) +
  `srv_peer_info` identity exposure + all userspace mirrors.
- **Audit (R1, opus prosecutor):** CLEAN -- 0 P0, 0 P1, 1 P2, 4 P3, all fixed in the
  close. F1 (supp-gid SYSTEM reject), F2 (clamp inherited count), F3 (proc_apply_identity
  self-validates reserved values), F4 (stale 56-byte comments), F5 (pouch mirror size
  assert). Carried OUT of scope at A-1a, **CLOSED by A-4-pre**: a pre-existing
  P5-hostowner I-2 hole -- `rfork_internal` did not `& ~CAP_ELEVATION_ONLY` despite caps.h
  saying it does (a hostowner-elevated parent could rfork `CAP_HOSTOWNER` to a child).
  A-4-pre added the 1-line strip + the `caps.rfork_strips_elevation_only` regression test
  (652/653 FAIL pre-fix, 653/653 post); did not touch identity.
- **Deferred:** A-1b corvus identity DB + `RESOLVE_ID`/`RESOLVE_NAME` + CRVS v2;
  self-de-escalation (drop to NONE) seam; A-2d kernel rwx permission check that
  *consumes* principal_id/groups; A-3 SO_PEERCRED uid/gid marshal + per-user stratumd
  presentation.

---

## Known caveats / footguns

- **srv_peer_info @20 is `alive`, not pad.** Anything mirroring this struct MUST place
  the identity fields after `alive` (principal_id@24). A short (24-byte) mirror is a
  16-byte stack/heap **clobber** when the kernel writes the 40-byte record — this was
  caught at A-1a (the stale pouch mirror faulted with `addr=0xfffffffefffffffe` =
  `SYSTEM | SYSTEM`). All in-tree mirrors are 40 bytes.
- **Identity is not authority.** Do not gate any privileged kernel operation on
  `principal_id` (I-22). The only consumer of identity-for-access is the future A-2d
  rwx check, which compares file mode/uid/gid against the Proc identity — and there is
  no uid-0 / SYSTEM bypass.
- **Inheritance is by value at fork.** A later identity change on a parent (there is
  none at v1.0) would not propagate to already-forked children.
