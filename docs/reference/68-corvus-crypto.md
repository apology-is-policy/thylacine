# 68 — corvus crypto: Argon2id + AEGIS-256 + state file (P5-corvus-bringup-c)

## Purpose

Records the third corvus sub-chunk: real cryptographic authentication.
P5-corvus-bringup-b's AUTH verb accepted any non-empty passphrase;
P5-corvus-bringup-c makes AUTH verify the passphrase through an
Argon2id-derived KEK + AEGIS-256 AEAD-unwrap of a wrapped keypair, and
adds the USER_CREATE verb that mints a user's wrapped state. Per
CORVUS-DESIGN.md §4.3 / §5.1.

## What landed

- **Pure-Rust crypto crates** pulled into the `aarch64-unknown-none`
  `usr/corvus` build: `argon2` (Argon2id KDF), `aegis` with the
  `pure-rust` feature (AEGIS-256 AEAD — the default build wraps a C
  `libaegis` that needs libc headers absent on the bare-metal target;
  `pure-rust` short-circuits `build.rs` and uses the software AES
  backend), `linked_list_allocator` (heap).
- **Heap.** Argon2id needs a working-memory matrix, so corvus gains a
  `#[global_allocator]` `LockedHeap` over a 24 MiB static BSS buffer,
  initialized at `rs_main` entry before the hardening sequence.
- **State file format** (`CorvusUserState`) per CORVUS-DESIGN.md §4.3:
  `magic 'CRVS'`, version, Argon2id params (`t_cost`, `m_cost_kib`,
  `parallelism`), 16-byte salt, 32-byte AEGIS nonce, ciphertext, 32-byte
  tag. Byte-precise `to_bytes` serializer + compile-time layout asserts.
  At this sub-chunk the records live in an **in-memory** `Vec` only —
  FS persistence lands when `/var/lib/corvus/` is mounted (P5+).
- **USER_CREATE (verb_id=5).** Parses `user + passphrase + backend`;
  generates a fresh salt + nonce via `t_getrandom`; runs
  `Argon2id(passphrase, salt)` → 32-byte KEK; generates a 64-byte
  placeholder keypair via CSPRNG (the ML-KEM-768 + X25519 hybrid
  keypair lands at a later sub-chunk); AEGIS-256-wraps the keypair under
  the KEK with AD = `"thylacine-corvus-v1" || user || backend_id`;
  stores the `CorvusUserState` record. Re-creating an existing user is
  refused (`PermissionDenied`).
- **AUTH (verb_id=1)** now real-crypto-gated: looks up the user's state,
  re-derives the KEK via Argon2id, AEGIS-256-unwraps with the stored
  nonce + AD; a tag mismatch returns `BadAuth`; success mints the
  session token + installs the session as before. The KEK and the
  unwrapped keypair are wiped (`write_volatile`) on every path.

## Argon2id parameters

v1.0 interactive preset: `t_cost=2, m_cost=16 MiB, parallelism=1`. The
CORVUS-DESIGN.md §4.3 default is `m_cost=64 MiB`; corvus-c caps it at
16 MiB because the heap is a single contiguous BSS-backed allocation and
the kernel buddy allocator (`burrow_create_anon`) rounds a 64 MiB heap
up to an order-14 contiguous block that strains the free-list after the
442-test suite has fragmented memory. `m_cost_kib` is a per-record
stored field, so a future chunk can raise the default — old records
still verify at their stored cost, no format break.

## Spec correspondence

`specs/corvus.tla`: AUTH = `AuthSuccess`; SESSION_CLOSE = `SessionClose`.
USER_CREATE is outside the spec's session state-machine scope (it is the
precondition that makes `AuthSuccess` reachable for a user) — a future
spec extension models the user-state ledger.

## Kernel-side change

`SYS_SPAWN_BLOB_MAX` 32 KiB → 256 KiB and `JOEY_BLOB_MAX` 32 KiB →
64 KiB: the corvus binary grew to ~109 KiB (embedded Argon2id +
AEGIS-256 + allocator) and joey's inline orchestration grew with the
USER_CREATE + AUTH(wrong) + AUTH(ok) + SESSION_CLOSE wire codec.

## Tests

Boot path is the regression. joey drives `USER_CREATE("michael", ...)` →
`AUTH(wrong passphrase)` (asserts `BadAuth` — exercises the AEGIS-256
tag-mismatch branch) → `AUTH(correct passphrase)` (asserts OK + token)
→ `SESSION_CLOSE`. Boot log on success:

```
joey: USER_CREATE michael ok
joey: AUTH(wrong pass) returned BadAuth (expected)
joey: AUTH ok (token=s...)
joey: SESSION_CLOSE ok
```

442/442 PASS × default + UBSan.

## Status

Landed at P5-corvus-bringup-c, on top of P5-el1h-kernel. The crypto
path is real (Argon2id + AEGIS-256); the keypair is a placeholder and
state is in-memory.

## Known caveats / deferred

- **Hybrid keypair.** The wrapped 64 bytes are CSPRNG placeholder, not
  the ML-KEM-768 + X25519 hybrid keypair of CORVUS-DESIGN.md — deferred
  to a later sub-chunk (it pairs with the UNWRAP verb).
- **FS persistence.** `CorvusUserState` records are in-memory; they do
  not survive a corvus restart. FS persistence lands when
  `/var/lib/corvus/` is a mounted tree.
- **Argon2id m_cost** is 16 MiB, below the 64 MiB design default — see
  above.
- **Not yet implemented**: the zxcvbn passphrase-entropy gate (C-12),
  CHANGE_PASSPHRASE, per-user rate-limiting (C-16), the audit log.
