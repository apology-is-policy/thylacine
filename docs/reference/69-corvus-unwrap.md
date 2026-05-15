# 69 — corvus WRAP/UNWRAP: ML-KEM-768 + X25519 hybrid keypair (P5-corvus-bringup-d)

## Purpose

The fourth corvus sub-chunk. P5-corvus-bringup-c wrapped a 64-byte CSPRNG
placeholder where the hybrid keypair belongs, and discarded it after
AUTH. P5-corvus-bringup-d makes the keypair real — an ML-KEM-768
(FIPS 203) + X25519 hybrid — has AUTH retain it in the mlock'd session
slab, and adds the two verbs that consume it: WRAP (verb_id=10) and
UNWRAP (verb_id=4). UNWRAP is the C-7-bearing verb: corvus refuses a DEK
unwrap for a (session, dataset) pair the session does not own. Per
CORVUS-DESIGN.md §6.4/§6.5 and STRATUM-API-V1.md §5.2.

## What landed

### Hybrid keypair

USER_CREATE generates a real hybrid keypair via `generate_hybrid_keypair()`:

- X25519 (`x25519-dalek`): a 32-byte secret + 32-byte public key.
- ML-KEM-768 (`ml-kem`, FIPS 203): an 1184-byte encapsulation (public)
  key + a 2400-byte decapsulation (secret) key.

The keypair plaintext is a fixed 3648-byte layout (`KEYPAIR_LEN`):

```
[0..32)      x25519 secret key
[32..64)     x25519 public key
[64..1248)   ML-KEM-768 encapsulation key      1184 B
[1248..3648) ML-KEM-768 decapsulation key      2400 B
```

Both public halves are stored so WRAP needs no re-derivation. The
keypair is AEGIS-256-wrapped under the Argon2id KEK into the
`CorvusUserState` ciphertext exactly as the -c placeholder was —
`KEYPAIR_LEN` is the only constant that changed (64 → 3648). The `CRVS`
format version stays 1: no on-disk CRVS file exists yet, and the
ciphertext length is implied by file size, so the resize breaks no
format.

### Crate integration

Pure-Rust crates joined the `aarch64-unknown-none` build: `ml-kem` 0.2
(FIPS 203 ML-KEM, RustCrypto; `zeroize` feature — typed secret keys
wipe on drop), `x25519-dalek` 2 (`static_secrets` +
`zeroize` features), `sha2` 0.10 (the KEK combiner), `kem` 0.3.0-pre.0
(the `Encapsulate`/`Decapsulate` traits ml-kem's keys implement —
ml-kem does not re-export them, so it is a direct dep) and `rand_core`
0.6 (the RNG trait). All take an explicit RNG, so no `getrandom` crate
is pulled (it has no bare-metal backend). `ThylaRng` adapts
`rand_core::RngCore` over `t_getrandom`; a post-startup `t_getrandom`
failure is fatal — corvus verified the CSPRNG seeded at startup, so a
later failure is an invariant violation.

### The DEK envelope (hybrid-PKE)

The `wrapped` blob WRAP produces and UNWRAP consumes is a corvus-owned
KEM-DEM hybrid (CORVUS-DESIGN.md §6.5). 1217 bytes:

```
[0]            envelope_version  u8 = 1
[1..1089)      mlkem_ct          1088 B
[1089..1121)   x25519_eph_pk     32 B
[1121..1153)   aead_nonce        32 B
[1153..1185)   dek_ct            32 B
[1185..1217)   dek_tag           32 B
```

- **KEM**: ML-KEM-768 encapsulate to the recipient encapsulation key →
  `(mlkem_ct, ss_pq)`; a fresh X25519 ephemeral → recipient-static ECDH
  → `ss_cl`.
- **KEK**: `SHA-256("thylacine-corvus-dek-kdf-v1" || ss_pq || ss_cl ||
  eph_pk || mlkem_ct)`. Hashing the ciphertext transcript binds the KEK
  to the exact envelope — a substituted ciphertext yields a different
  KEK and the AEAD tag then fails.
- **DEM**: AEGIS-256 over the 32-byte DEK, AD = `"thylacine-corvus-dek-v1"
  || dataset || key_id` (key_id is the LE u64 from the frame). The
  dataset + key_id bind means a wrapped DEK cannot be replayed against
  another dataset or key generation.

The hybrid stays secure if *either* the post-quantum or the classical
KEM holds. ML-KEM decapsulation is FIPS 203 implicit-rejection (a bad
ciphertext yields a deterministic-but-wrong shared secret, never an
error) — the AEGIS-256 tag check is the sole integrity gate.

### WRAP (verb_id=10) and UNWRAP (verb_id=4)

WRAP wraps a 32-byte DEK under the session user's hybrid public key;
UNWRAP recovers the DEK with the secret key. Both:

1. Verify the 33-byte session token (constant-time compare).
2. **C-7 gate**: look the dataset up in the ownership table — refuse
   `NotFound` if unknown, `PermissionDenied` if the owner is not the
   session's bound user. For UNWRAP this fires *before any crypto*.
3. Copy the keypair out of the session slab, run the envelope
   wrap/unwrap, volatile-wipe the slab copy.

The session slab gained a `keypair: [u8; 3648]` field — AUTH installs
the AEGIS-unwrapped keypair there (corvus-c discarded it); WRAP/UNWRAP
copy it per call; `session_clear()` volatile-wipes it.

### Dataset-ownership table

An in-memory `(dataset, owner)` table, populated at USER_CREATE
(`users/<name>` → `<name>`). The C-7 gate's authority. FS persistence
(loading `/var/lib/corvus/datasets/`) is gated on that tree being
mounted (P5+).

## Kernel-side change — user stack 16 KiB → 256 KiB

`EXEC_USER_STACK_SIZE` (`kernel/include/thylacine/exec.h`) grew from
16 KiB to 256 KiB. ML-KEM-768 keygen and especially decapsulate (the
FIPS 203 FO re-encryption) use tens of KiB of stack; the prior 16 KiB
user-stack VMA overflowed. 256 KiB is generous headroom for every
userspace Proc. The fixed size remains the v1.0 placeholder — Phase 5+
replaces it with demand-grow on stack faults.

## Error paths

| Verb | Status | Trigger |
|---|---|---|
| WRAP | `OK` | envelope returned (1217-byte payload) |
| WRAP | `BadFormat` | malformed frame; `dek_len != 32` |
| WRAP | `BadAuth` | token mismatch / no active session |
| WRAP | `NotFound` | dataset not in the ownership table |
| WRAP | `PermissionDenied` | session user is not the dataset owner (C-7) |
| WRAP | `InternalError` | keypair-copy failure or crate size mismatch |
| UNWRAP | `OK` | DEK returned (32-byte payload) |
| UNWRAP | `BadFormat` | malformed frame, or a structurally-malformed envelope (wrong length / version byte) |
| UNWRAP | `BadAuth` | token mismatch / no active session |
| UNWRAP | `NotFound` | dataset not in the ownership table |
| UNWRAP | `PermissionDenied` | session user is not the dataset owner (C-7) |
| UNWRAP | `InternalError` | AEGIS-256 tag failure (wrong keypair / corrupted blob) |

## Spec correspondence

`specs/corvus.tla`: UNWRAP maps to the `Unwrap(p, d)` action; the C-7
gate is the `UnwrapOwnerOnly` invariant; the cross-user refusal is the
`BuggyUnwrapCrossUser` negative. WRAP is the inverse of UNWRAP, C-7-gated
identically — `UnwrapOwnerOnly` already pins the authorization shape, so
WRAP is not modeled as a separate action (documented in the corvus.tla
header).

## Tests

joey drives the round-trip on every boot: USER_CREATE michael →
USER_CREATE susan → AUTH(wrong)→BadAuth → AUTH(ok)→token →
WRAP(users/michael, dek) → UNWRAP(users/michael)→DEK byte-matches →
UNWRAP(users/susan)→PermissionDenied (the spec-pinned C-7 test —
michael's session denied susan's dataset) → UNWRAP(users/ghost)→NotFound
→ SESSION_CLOSE. 442/442 PASS × default + UBSan, 0 `EXTINCTION` lines.

## Status

Landed at P5-corvus-bringup-d. The crypto is real end to end (hybrid
keypair + hybrid-PKE envelope). State is in-memory; FS persistence and
the admin verbs (CHANGE_PASSPHRASE, USER_DELETE, ADMIN_ELEVATE, RECOVER,
ROTATE_KEY) are later sub-chunks.

## Known caveats / deferred

- **USER_CREATE is not yet capability-gated.** Any peer holding the
  corvus pipe can create users; the `CapHostowner` gate (the spec's
  `AdminVerb` precondition) lands with ADMIN_ELEVATE / P5-hostowner.
  `MAX_USERS` (256) bounds the in-memory user table so the ungated
  verb cannot OOM the daemon; per-user rate-limiting (C-16) is also
  deferred.
- **Userspace-stack guard page (audit F7 — CLOSED by
  P5-secondary-stack-guard).** The 256 KiB user-stack VMA now has a
  one-page guard VMA directly below it (`prot==0`, no BURROW, installed
  by `exec_map_user_stack`). An overflow past 256 KiB faults instead of
  corrupting a lower VMA, and `vma_insert`'s overlap rejection reserves
  the page against a future mapping allocator. See
  `docs/reference/27-exec.md` §"User-stack guard page".
- **State + ownership table are in-memory.** They do not survive a
  corvus restart; FS persistence lands with `/var/lib/corvus/`.
- **WRAP is C-7-gated** — a session can only wrap for datasets it owns.
  Real dataset provisioning (§5.4) may later need an admin WRAP path
  for not-yet-logged-in users — a future verb refinement.
- **`key_id`** is bound into the DEK-envelope AAD (so the v1 envelope
  is rotation-safe), but is not yet used to *select* among multiple
  keys for a dataset — multi-key datasets / rotation are v1.x.
- **Argon2id m_cost** stays 16 MiB (the -c contiguous-BSS-heap cap),
  below the §4.3 64 MiB default — unchanged from P5-corvus-bringup-c.
