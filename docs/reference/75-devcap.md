# 75 — devcap: the hostowner-elevation `cap` device

**Status**: implemented at P5-hostowner-b-a (kernel side; pending fixup commit). The userspace consumer (corvus ADMIN_ELEVATE + joey redemption + admin-verb gating) lands at P5-hostowner-b-b.

## Purpose

`devcap` is the kernel-side counterpart of corvus's factotum-pattern hostowner elevation (CORVUS-DESIGN.md §5.5/§5.5.1). corvus is userspace and cannot mutate a Proc's capability set directly — elevation is a **two-phase, file-mediated grant** through this kernel device.

Plan 9 lineage: `cap(3)` — the `caphash` / `capuse` pair through which factotum conferred authority on a process. The Thylacine adaptation differs in two ways: (1) Plan 9's device changed a process's user identity (uid strings); Thylacine has no uid 0, so the device grants a *capability bit* and binds the grant to the unforgeable per-Proc `stripes` identity (CORVUS-DESIGN.md §6.3). (2) Plan 9 used an HMAC over a factotum↔kernel shared secret because factotum's channel was untrusted; here the `grant` write is itself `CAP_GRANT_HOSTOWNER`-gated, so the kernel trusts the channel directly — no token, no kernel-side cryptography.

## Public API

The device exposes two write-only files under `/cap`:

```
/cap/grant   — write-only; gated on writer holds CAP_GRANT_HOSTOWNER
/cap/use     — write-only; gated on writer holds PROC_FLAG_CONSOLE_ATTACHED
               AND a non-expired pending grant exists for the writer's stripes
               with a matching cap_mask
```

Wire format (message-oriented; one write = one fixed-size frame):

```
/cap/grant   — 16 bytes: cap_mask (u64 LE) + target_stripes (u64 LE)
/cap/use     —  8 bytes: cap_mask (u64 LE)
```

A successful write returns the frame length (`CAP_GRANT_WRITE_LEN = 16` or `CAP_USE_WRITE_LEN = 8`). Any short / oversized / gate-failing / lookup-failing write returns `-1`. Partial writes are not supported (each frame is atomic).

Kernel-internal core API (`kernel/include/thylacine/devcap.h`):

```c
long cap_register_grant_for_writer(struct Proc *writer,
                                   caps_t cap_mask, u64 target_stripes);
long cap_redeem_grant_for_writer(struct Proc *writer, caps_t cap_mask);
void cap_proc_exit_notify(struct Proc *p);   // called from exits()
int  cap_pending_count(void);                // tests + diagnostics
void cap_reset_table(void);                  // test-only
```

The Dev `write` op (`devcap_write`) is a thin shim that resolves the current Proc via `current_thread()->proc` and dispatches to the right core based on the leaf Spoor's aux magic. Tests call the cores directly.

## Implementation

**File**: `kernel/devcap.c` (~360 LOC). **Header**: `kernel/include/thylacine/devcap.h`. **Dev registration**: `kernel/dev.c::dev_init` (`dev_register(&devcap)` after `devsrv`). **Proc-exit hook**: `kernel/proc.c::exits()` (after `srv_proc_exit_notify`).

### Pending-grant table

```c
enum cap_grant_state { CAP_GRANT_FREE = 0, CAP_GRANT_PENDING = 1 };

struct cap_grant_entry {
    enum cap_grant_state state;
    caps_t  cap_mask;
    u64     target_stripes;
    u64     expiry_ns;
};

static struct cap_grant_table {
    spin_lock_t            lock;
    struct cap_grant_entry entries[CAP_GRANT_MAX];   // CAP_GRANT_MAX = 16
} g_cap_grants;
```

Capacity (`CAP_GRANT_MAX = 16`) is generous for v1.0's single-console workload — a pending grant is short-lived (one elevation in flight per console session, expires in 30 s). BSS zero-init reads as all FREE.

Expiry (`CAP_GRANT_EXPIRY_NS = 30 s`) is the window between corvus's `/cap/grant` write and joey's `/cap/use` redemption. joey reads corvus's OK response synchronously and immediately writes `/use` — 30 s comfortably covers any scheduling jitter while keeping a stale grant from persisting indefinitely.

Lazy expiry: `cap_find_free_locked(now_ns)` scans the table once, freeing any entry whose expiry has passed, while looking for an open slot. Both `cap_register_grant_for_writer` and `cap_pending_count` walk through this; `cap_redeem_grant_for_writer` checks the matched entry's `expiry_ns <= now` and treats expired as not-found (clearing the slot in passing).

Re-register semantics (CORVUS-DESIGN.md §5.5.1: "at most one pending grant per tag; a re-register replaces it"): `cap_register_grant_for_writer` looks up `target_stripes` first; if a PENDING entry exists, it overwrites it in place rather than allocating a new slot.

### Gates

`/cap/grant` writer gate — `cap_register_grant_for_writer`:
- `writer->caps & CAP_GRANT_HOSTOWNER` must be set.
- `cap_mask` must be subset of `CAP_GRANTABLE` (v1.0: only `CAP_HOSTOWNER`).
- `cap_mask` must be non-zero.
- `target_stripes` must be non-zero (the reserved fail-closed sentinel).

`/cap/use` writer gate — `cap_redeem_grant_for_writer`:
- `proc_is_console_attached(writer)` must be true.
- `cap_mask` must be non-zero and subset of `CAP_GRANTABLE`.
- `proc_stripes(writer)` must be non-zero.
- A PENDING entry must exist for `proc_stripes(writer)`.
- That entry's `expiry_ns > timer_now_ns()`.
- That entry's `cap_mask` must EQUAL the requested `cap_mask` (not subset — explicit-equality keeps the protocol unambiguous; a mismatch fails closed and does NOT consume the grant).

On success: `writer->caps |= cap_mask`; the entry is cleared (one-shot). v1.0 single-thread-per-Proc makes the direct `|=` race-free; a multi-thread-per-Proc lift needs `__atomic_fetch_or`.

### Defense in depth

The two gates live in different trust domains:

- **`/grant` requires `CAP_GRANT_HOSTOWNER`** — a fork-grantable cap that joey holds via `CAP_ALL` and confers on corvus in its spawn mask. Ordinary user Procs never receive it. The kernel-enforced answer to "only corvus may register grants."
- **`/use` requires `PROC_FLAG_CONSOLE_ATTACHED`** — a one-way kernel-stamped bit set only on Procs spawned through joey's console-login chain, never propagated across `rfork`. The kernel-enforced answer to "only a console session may be elevated."

A compromised corvus can register grants for arbitrary `target_stripes`, but the kernel only lets a *console-attached* writer redeem. corvus elevating a network process is structurally impossible; a corvus compromise is bounded to the local physical console. This is what makes `specs/corvus.tla::HostownerRequiresConsole` robust against corvus's own correctness.

### Proc-exit cleanup

`cap_proc_exit_notify(p)` is called from `kernel/proc.c::exits()` for every exiting Proc (mirroring `srv_proc_exit_notify`'s pattern from devsrv). It drops any PENDING entry whose `target_stripes == proc_stripes(p)`.

Safety doesn't strictly require this — `stripes` are fresh per Proc (immutable, never recycled while the Proc lives), so a grant for an exited Proc cannot accidentally elevate a different Proc with a recycled pid. The cleanup is hygiene: it frees the table slot promptly so a long-lived series of unconsumed grants doesn't starve the bounded pool.

## Data structures

```c
#define CAP_GRANT_MAX            16u
#define CAP_GRANT_EXPIRY_NS      (30ull * 1000ull * 1000ull * 1000ull)  // 30 s
#define CAP_GRANTABLE            (CAP_HOSTOWNER)
#define CAP_GRANT_WRITE_LEN      16u    // 8B cap_mask + 8B target_stripes
#define CAP_USE_WRITE_LEN         8u    // 8B cap_mask

#define DEVCAP_GRANT_MAGIC       0x444341505F47524EULL  // 'DCAP_GRN'
#define DEVCAP_USE_MAGIC         0x444341505F555345ULL  // 'DCAP_USE'

struct devcap_leaf_ref {
    u64 magic;     // DEVCAP_GRANT_MAGIC or DEVCAP_USE_MAGIC
};
```

Each walked leaf Spoor (`/cap/grant` or `/cap/use`) carries a kmalloc'd `devcap_leaf_ref` in `c->aux`, holding only the magic that distinguishes which leaf. The Dev `write` op reads the magic to dispatch.

## Spec cross-reference

- `specs/corvus.tla::HostownerGrant` — the elevation step that produces a `CapHostowner`-holding Proc.
- `specs/corvus.tla::HostownerRequiresConsole` — the invariant the device structurally upholds.
- `specs/handles.tla::ElevationOnly` + `RforkStripsElevation` — the elevation-only cap axis; ensures `CAP_HOSTOWNER` never propagates across `rfork`.

The cap device's runtime is the *mechanism* that realizes these spec actions; no new invariants are added at this layer. Per the project-wide spec-to-code suspension (CLAUDE.md "Spec-to-code suspended until further notice"), no exhaustive clean-cfg TLC re-verification was run for this chunk; the buggy cfgs remain GREEN under the unchanged spec.

## Tests

`kernel/test/test_devcap.c` — 14 cases:

- `devcap.registered` — devcap in bestiary (dc='k', name="cap"); attach produces QTDIR root.
- `devcap.walk_grant_use` — walk to "grant"/"use" yields QTFILE leaves with correct magic.
- `devcap.walk_unknown` — walk to an unknown name returns NULL.
- `devcap.open_writeonly` — OREAD fails; OWRITE succeeds.
- `devcap.grant_gate_no_cap` — Proc without `CAP_GRANT_HOSTOWNER` rejected.
- `devcap.grant_gate_bad_args` — zero stripes / zero mask / non-grantable cap rejected.
- `devcap.grant_replace` — re-register for same stripes replaces in place; pending count stays 1.
- `devcap.grant_table_full` — `CAP_GRANT_MAX+1` distinct registrations; last fails.
- `devcap.use_gate_no_console` — writer without `PROC_FLAG_CONSOLE_ATTACHED` rejected; caps unchanged.
- `devcap.use_no_pending` — no grant exists; rejected.
- `devcap.use_basic` — grant + use → writer gains `CAP_HOSTOWNER`; pending count drops to 0.
- `devcap.use_one_shot` — second use after consume rejected (grant gone).
- `devcap.use_mismatched_cap` — grant for one cap, use for another rejected; grant retained.
- `devcap.exit_clears_grant` — `cap_proc_exit_notify` drops target's pending grant.

Test count: 511 → 525 (+14) × default + UBSan, 0 UBSan runtime errors.

## Error paths

`/cap/grant` write `-1` causes:
- Spoor is the root (wrong leaf), or aux has unknown magic.
- `n != CAP_GRANT_WRITE_LEN` (must be exactly 16 bytes).
- `current_thread()->proc->caps & CAP_GRANT_HOSTOWNER == 0` (gate fail).
- `cap_mask == 0`.
- `cap_mask & ~CAP_GRANTABLE != 0` (asked for non-elevation-only cap).
- `target_stripes == 0` (sentinel).
- Table full of non-expired entries.

`/cap/use` write `-1` causes:
- Spoor is the root, or aux has unknown magic.
- `n != CAP_USE_WRITE_LEN` (must be exactly 8 bytes).
- `cap_mask == 0` or `cap_mask & ~CAP_GRANTABLE != 0`.
- `!proc_is_console_attached(writer)`.
- `proc_stripes(writer) == 0`.
- No PENDING entry for writer's stripes.
- Matched entry expired (cleared as a side effect).
- Matched entry's `cap_mask != cap_mask` (grant retained on mismatch).

## Performance characteristics

The grant table is a 16-entry linear-scan structure. `cap_register_grant_for_writer` and `cap_redeem_grant_for_writer` each take the lock once and scan the table once — O(16) under the lock. At this scale a hash structure would be overkill; the cache footprint of 16 × `sizeof(struct cap_grant_entry)` (~40 bytes each) is one cache line.

The expiry sweep happens lazily on every `cap_register_grant_for_writer` and `cap_pending_count`. No background timer; no async cleanup. Stale entries occupy a slot only until the next operation walks the table.

## Status

Implemented + green at the P5-hostowner-b-a commit. Default + UBSan both pass 525/525.

The Dev `write` op resolves the current Proc via `current_thread()->proc`; the cores (`cap_register_grant_for_writer` / `cap_redeem_grant_for_writer`) take an explicit Proc * so tests can construct synthetic peers.

The userspace consumer lands at P5-hostowner-b-b: corvus's `ADMIN_ELEVATE` verb (`verb_id=7`) writes `/cap/grant`; joey's post-OK redemption writes `/cap/use`; the admin verbs (`USER_CREATE`, `USER_DELETE`, `WRAP` admin path, `ROTATE_KEY`) gate on the peer's live `caps & CAP_HOSTOWNER` via `SYS_SRV_PEER` (per the C-22 discipline — re-query per call, never cache).

## Known caveats / footguns

- **Re-register replaces in place** — a second `/grant` for the same `target_stripes` overwrites the previous entry (cap_mask + expiry both refreshed). If two distinct callers race a `/grant` for the same target, the last write wins. At v1.0 only corvus writes `/grant` (gated by `CAP_GRANT_HOSTOWNER`), so this is not an attack surface; documented for future multi-grantor scenarios.
- **Equality, not subset, on `/use`** — the redeem `cap_mask` must equal the granted `cap_mask`. A subset would be more flexible but less explicit; equality keeps the protocol unambiguous when the grantable set widens in v1.x.
- **No `/use` for arbitrary Procs** — the redeemer is the writer (`current_thread()->proc`), not a parameter. A grant for target T can only be redeemed by *T itself*. This is the C-22-equivalent "kernel writes current->caps, never a cross-Proc cap write."
- **Pending grants cost 16 slots** — a corvus that registers many concurrent grants without redemption could exhaust the table. At v1.0 corvus issues one grant per ADMIN_ELEVATE and joey redeems immediately, so this is not a practical concern. The 30 s expiry caps worst-case occupancy at 16 grants/30 s; a malicious corvus could try to wedge legitimate users, but only the gate-holder (CAP_GRANT_HOSTOWNER) can write `/grant` — joey grants this only to corvus.

## Naming rationale

`devcap` is the obvious name (the device is the cap-grant device). The `dc='k'` choice is constrained: `c` is taken by `devcons`, `C` by `devctl`. `k` for "kapability" is short, mnemonic, and free. Plan 9 used `#¤` (the currency-sign character) for `cap`; Thylacine sticks to ASCII for the device-character convention.
