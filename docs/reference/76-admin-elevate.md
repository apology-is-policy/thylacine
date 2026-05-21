# 76 — corvus ADMIN_ELEVATE + admin-verb gating

**Status**: implemented at P5-hostowner-b-b. End-to-end through joey's boot test.

## Purpose

The userspace half of the hostowner elevation flow (CORVUS-DESIGN.md §5.5/§5.5.1). The kernel-side `cap` device landed at P5-hostowner-b-a (`docs/reference/75-devcap.md`); this chunk wires the corvus consumer (`ADMIN_ELEVATE` verb), joey's redemption (`t_cap_use`), and the admin-verb gating that consumes the resulting `CAP_HOSTOWNER` bit.

## ADMIN_ELEVATE verb (verb_id=7)

### Wire format (CORVUS-DESIGN.md §6.4)

Request payload (binary frame):
```
[0..33)      token             33 B   (session token from AUTH)
[33..35)    sys_pass_len      u16 LE
[35..)      sys_passphrase    sys_pass_len bytes
```

Response payload: empty. Status codes:
- `STATUS_OK` (0) — elevation registered.
- `STATUS_BAD_FORMAT` (5) — malformed length / over-sized passphrase.
- `STATUS_BAD_AUTH` (1) — token unknown OR wrong system passphrase.
- `STATUS_PERMISSION_DENIED` (2) — peer is not console-attached.
- `STATUS_INTERNAL_ERROR` (6) — cap grant registration failed (e.g., the kernel grant-table is full; corvus lacks `CAP_GRANT_HOSTOWNER` due to spawn-mask misconfiguration).

### Handler flow (`usr/corvus/src/main.rs::handle_admin_elevate`)

```
verify payload length + passphrase length bounds
verify session_token_matches(token)                 → else BadAuth
verify conn.peer.console == 1                       → else PermissionDenied
verify sys_passphrase EQUALS SYSTEM_PASSPHRASE      → else BadAuth
                                                      (byte-compare, constant time over the equal-length window)
t_cap_grant(T_CAP_HOSTOWNER, conn.peer.stripes)     → else InternalError
return OK
```

Two gates in two trust domains:

- **Corvus verifies the system passphrase** — a check the kernel has no notion of.
- **The kernel verifies console-attached** — re-verified at `/cap/use` redemption. A compromised corvus could try to register grants for arbitrary `stripes`, but only a console-attached writer can actually redeem (the kernel-enforced gate, CORVUS-DESIGN.md §5.5.1 "defense in depth").

### v1.0 system passphrase (PLACEHOLDER)

`SYSTEM_PASSPHRASE` is currently the literal byte string `"thylacine"`, hardcoded in `usr/corvus/src/main.rs`. This is the placeholder that lets the boot test exercise the elevation mechanism end-to-end without standing up CRVS persistence + an installer flow.

The eventual mechanism (CORVUS-DESIGN.md §5.5) is:

```
system_KEK   = argon2id(system_passphrase, system_salt, sensitive_params)
system_wrap  = AEGIS256-wrap(canonical_magic) under system_KEK
verify       = AEGIS256-unwrap(system_wrap) under derived_KEK; check magic
```

This requires:
1. CRVS file persistence (to store `system_salt` + `system_wrap` across reboots).
2. An installer-driven first-boot setup (to set the initial passphrase and seal `system_wrap`).
3. A separate ADMIN_INITIALIZE (or RECOVER, §5.6) verb for set / rotate.

All of this lands when CRVS persistence does. Until then: the hardcoded passphrase is what makes the boot test deterministic.

### Why the passphrase isn't `_Static_assert`-pinned in the spec

The spec models the passphrase as opaque "valid/invalid" — `BUGGY_ELEVATE_WITHOUT_CONSOLE` already covers the console-attached failure mode, and `BUGGY_ADMIN_WITHOUT_PROC_CAP` covers the post-elevation gate. The passphrase itself is part of the corvus-internal check `specs/corvus.tla::AdminElevate` abstracts as a precondition; the byte-compare-against-`SYSTEM_PASSPHRASE` is the v1.0 instantiation.

## Joey: ADMIN_ELEVATE + t_cap_use bootstrap (`usr/joey/joey.c`)

### Spawn mask change

`joey::main` now confers `T_CAP_GRANT_HOSTOWNER` on corvus in its `t_spawn_with_perms` mask — this is the fork-grantable cap the kernel checks in `cap_register_grant_for_writer`. Without it corvus's ADMIN_ELEVATE handler returns `STATUS_INTERNAL_ERROR` (`t_cap_grant` is rejected at the kernel gate).

```c
t_spawn_with_perms(
    "corvus", 6, no_fds, 0,
    T_CAP_LOCK_PAGES | T_CAP_CSPRNG_READ | T_CAP_GRANT_HOSTOWNER,
    T_SPAWN_PERM_MAY_POST_SERVICE);
```

### Boot sequence (revised)

```
1. USER_CREATE michael                  (bootstrap exception; see below)
2. AUTH michael (wrong)                 → BadAuth (test fail path)
3. AUTH michael (correct)               → 33-byte session token
4. ADMIN_ELEVATE michael                → OK (corvus writes /cap/grant)
5. t_cap_use(T_CAP_HOSTOWNER)           → OK (joey's Proc gets CAP_HOSTOWNER)
6. USER_CREATE susan                    → OK (gated now; joey holds the cap)
7. WRAP / UNWRAP × 5 / SESSION_CLOSE     (unchanged)
8. Q11 negative + reconnect              (unchanged)
```

### Bootstrap exception (handle_user_create)

`USER_CREATE` requires the caller's connection peer to hold `CAP_HOSTOWNER` (gated via fresh `SYS_SRV_PEER`, C-22), EXCEPT when no users yet exist — the first creation is free so the initial hostowner candidate can be created without authorization. Once the user table is non-empty, every subsequent creation requires the live cap.

This bootstraps the hostowner chicken-and-egg: `ADMIN_ELEVATE` requires an authenticated session, an authenticated session requires a user, a user requires `USER_CREATE`, `USER_CREATE` requires `CAP_HOSTOWNER`, and `CAP_HOSTOWNER` is only conferred by `ADMIN_ELEVATE`. The "no users yet → free" exception cuts the cycle.

Single-threaded corvus means there's no TOCTOU race on `user_states_count() > 0` between the gate check and the body.

## Admin-verb gating: USER_CREATE (the C-22 pattern)

`handle_user_create` is the v1.0 first instance of the C-22 pattern (CORVUS-DESIGN.md §6.3):

```rust
unsafe fn peer_live_caps(handle: i64) -> u64 {
    let mut info = TSrvPeerInfo::default();
    if t_srv_peer(handle, &mut info) != 0 { return 0; }
    if info.alive == 0                    { return 0; }
    info.caps
}

unsafe fn handle_user_create(handle: i64, payload: &[u8], response: &mut Vec<u8>) {
    if user_states_count() > 0 {
        let caps = peer_live_caps(handle);
        if (caps & T_CAP_HOSTOWNER) == 0 {
            return stage_response(response, STATUS_PERMISSION_DENIED, &[]);
        }
    }
    // ... existing body ...
}
```

Key properties:

- **Fresh `t_srv_peer` per call** — never cache `caps` on per-conn state; peer caps mutate via `/cap/use` mid-conversation.
- **Fail-closed on syscall failure** — `peer_live_caps` returns 0 on any error; a 0 mask fails the bit test.
- **Fail-closed on dead peer** — `info.alive == 0` also yields 0 (the kernel's dead-Proc guard surfaces here).

The same pattern applies to `USER_DELETE`, `ROTATE_KEY`, and the `WRAP` admin path when those land — each calls `peer_live_caps(handle)` before the irreversible effect.

## Public API additions

### Kernel syscalls (`kernel/include/thylacine/syscall.h`)

```c
SYS_CAP_GRANT    = 32,   // arg: cap_mask (x0), target_stripes (x1)
SYS_CAP_USE      = 33,   // arg: cap_mask (x0)
```

Returns 0 on success, -1 on any gate fail / bad args / table full / no pending grant.

### libt (`usr/lib/libt/include/thyla/syscall.h`)

```c
#define T_CAP_HOSTOWNER       (1UL << 3)   // elevation-only; not in CAP_ALL
#define T_CAP_GRANT_HOSTOWNER (1UL << 4)   // fork-grantable; joey → corvus

long t_cap_grant(unsigned long cap_mask, unsigned long target_stripes);
long t_cap_use(unsigned long cap_mask);
```

### libthyla-rs (`usr/lib/libthyla-rs/src/lib.rs`)

```rust
pub const T_SYS_CAP_GRANT: u64        = 32;
pub const T_SYS_CAP_USE: u64          = 33;
pub const T_CAP_HOSTOWNER: u64        = 1 << 3;
pub const T_CAP_GRANT_HOSTOWNER: u64  = 1 << 4;

pub unsafe fn t_cap_grant(cap_mask: u64, target_stripes: u64) -> i64;
pub unsafe fn t_cap_use(cap_mask: u64) -> i64;
```

### Why direct syscalls, not Dev write

The cap device exposes `/cap/grant` and `/cap/use` as write-only files. The Dev `write` op (`devcap_write` in `kernel/devcap.c`) is the eventual production path through a future namespace-aware `open` syscall. At v1.0 there is no userspace `t_open`, so the two writers reach the cores directly via these syscalls. Both paths converge on the same kernel functions (`cap_register_grant_for_writer` / `cap_redeem_grant_for_writer`); the gate semantics are identical. When `t_open` lands, the syscalls become alternative front-ends to the same mechanism, or get retired entirely.

## Spec cross-reference

- `specs/corvus.tla::AdminElevate` — the spec action ADMIN_ELEVATE realizes.
- `specs/corvus.tla::HostownerRequiresConsole` — the invariant the console-attached gate upholds (with the kernel-side `/cap/use` gate as the load-bearing enforcement).
- `specs/corvus.tla::AdminRequiresProcCap` — the invariant the `handle_user_create` gate upholds (admin verbs require `CapHostowner` on the peer's live `caps`).

Per the project-wide spec-to-code suspension (CLAUDE.md), no clean-cfg TLC re-verification was run for this chunk; the 8 buggy cfgs remain GREEN under the unchanged spec.

## Tests

No new kernel-internal tests beyond what P5-hostowner-b-a landed. The end-to-end behavior is exercised by joey's boot test:

```
joey: USER_CREATE michael ok (bootstrap)
joey: AUTH(wrong pass) returned BadAuth (expected)
joey: AUTH ok (token=...)
joey: ADMIN_ELEVATE ok
joey: t_cap_use(CAP_HOSTOWNER) ok (joey now hostowner)
joey: USER_CREATE susan ok (gated on CAP_HOSTOWNER)
joey: WRAP users/michael ok
joey: UNWRAP users/michael ok
joey: UNWRAP users/susan refused PermissionDenied (C-7 verified)
... rest unchanged ...
```

Test count unchanged at 525/525 PASS × default + UBSan (the new behavior is integration coverage, not kernel-internal unit tests).

## Error paths

`ADMIN_ELEVATE` returns non-OK on:
- Token doesn't match any active session → `BAD_AUTH`.
- Peer not console-attached → `PERMISSION_DENIED`.
- System passphrase mismatch (wrong length OR wrong bytes) → `BAD_AUTH`.
- `t_cap_grant` syscall fails (kernel grant-table full; corvus lacks `CAP_GRANT_HOSTOWNER`; bad cap_mask) → `INTERNAL_ERROR`.

`USER_CREATE` returns non-OK on:
- Caller lacks `CAP_HOSTOWNER` AND the user table is non-empty → `PERMISSION_DENIED` (new at this chunk).
- Existing pre-this-chunk failure modes (BAD_FORMAT / PERMISSION_DENIED for duplicate user / INTERNAL_ERROR for OOM) unchanged.

## Known caveats / footguns

- **`SYSTEM_PASSPHRASE` is hardcoded** — `"thylacine"`. Anyone with source-tree access can elevate. Replace this when CRVS persistence + the installer flow land.
- **No rate limiting on ADMIN_ELEVATE** — a malicious user (with an authenticated session) can brute-force the system passphrase by repeatedly calling ADMIN_ELEVATE. The hardcoded passphrase makes this academic at v1.0; the eventual Argon2id check would be intrinsically expensive enough to bound brute force, but explicit rate limiting is owed work.
- **The bootstrap exception is permanent for the empty-table case** — re-deleting all users would re-open the bootstrap window. v1.0 has no USER_DELETE so this is not yet a hazard; document the requirement for USER_DELETE's landing.
- **Peer-caps cached snapshot is fine for ADMIN_ELEVATE** — `conn.peer.console` is immutable (one-way kernel-stamped, never propagated by rfork), so the at-accept snapshot is authoritative for the console check. The C-22 fresh-query discipline applies to MUTABLE fields (`caps`); ADMIN_ELEVATE doesn't read those.
- **No cap-drop syscall at v1.0** — `caps` only grows. Pre-elevation a Proc has `caps & CAP_HOSTOWNER == 0`; post-elevation always set. A future cap-drop API would need to integrate with the C-22 re-query discipline (the design's `caps` field would then move bidirectionally).

## Status

Implemented + green at the P5-hostowner-b-b commit. Boot test exercises every production-path:

- joey's spawn confers `CAP_GRANT_HOSTOWNER` on corvus.
- USER_CREATE michael uses the bootstrap exception.
- AUTH establishes the session.
- ADMIN_ELEVATE verifies console + passphrase, registers the grant via SYS_CAP_GRANT.
- t_cap_use (SYS_CAP_USE) redeems the grant, joey's Proc gains CAP_HOSTOWNER.
- USER_CREATE susan exercises the C-22-equivalent fresh-caps re-query gate.
- All subsequent verbs (WRAP, UNWRAP × N, SESSION_CLOSE, Q11 reconnect) remain green.

The corvus-d audit's deferred F2-gate-half (USER_CREATE not CapHostowner-gated) is now CLOSED.

## What lands next

- **P5-hostowner-c** — RECOVER verb (CORVUS-DESIGN.md §5.6). The recovery-phrase rotation flow. Requires CRVS persistence (system_salt + system_wrap + recovery_wrap stored across reboots); likely couples with the installer-driven setup that retires `SYSTEM_PASSPHRASE`'s hardcoded placeholder.
- **WRAP admin path** — the dataset-provisioning WRAP from §5.4 (where the hostowner seals a DEK for a not-yet-logged-in user). Same `peer_live_caps & CAP_HOSTOWNER` gate.
- **USER_DELETE** + **ROTATE_KEY** — same gate.

## Naming rationale

`ADMIN_ELEVATE` follows the existing CORVUS-DESIGN verb-name convention (uppercase, snake-case). `t_cap_grant` / `t_cap_use` match the design's `/cap/grant` and `/cap/use` file names — when `t_open` lands and the Dev write op becomes the production path, the syscalls retire but the names remain consistent with the file paths.
