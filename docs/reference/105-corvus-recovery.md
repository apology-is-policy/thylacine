# 105 — corvus recovery keyslot (A-5c-a + A-5c-b + A-5c-c)

## Purpose

The recovery keyslot is corvus's disaster-recovery instrument: a **second
wrap of a subject's hybrid keypair**, alongside the passphrase wrap
(`hybrid.corvus`), under a key derived from a 24-word **BIP-39 recovery
phrase**. It lets a user who has forgotten their passphrase reset it from the
printed phrase — the LUKS two-keyslot model applied to corvus's keypair.

The defining property is that recovery re-wraps the **keypair**, not the
dataset DEK. The keypair *value* is unchanged across a recovery, so its public
keys are unchanged, so every per-dataset DEK envelope (encapsulated to those
public keys and stored in Stratum) stays valid. A-5c therefore has **no
kernel and no Stratum surface** — it is corvus-side crypto plus a login UX
flow (the UX is A-5c-c).

A-5c-a implements the **user** subject (`recovery.corvus`, the per-user
keyslot) and the `RECOVER(subject_kind=1)` verb. A-5c-b adds the **system**
subject (`system-recovery-wrap`, = P5-hostowner-c): the admin hybrid keypair is
host-baked at build time (`corvus-mint` -> `system-wrap` + `system-recovery-wrap`)
and `ADMIN_ELEVATE` becomes a real Argon2id+AEGIS unwrap of `system-wrap` (the
v1.0 `b"thylacine"` byte-compare is retired). See "A-5c-b" below.

Design: `CORVUS-DESIGN.md` §5.5.1 + §5.6 + §8 + §9 (C-20/C-27/C-28);
`IDENTITY-DESIGN.md` §9.9. Scripture-first commits `064807c` (A-5c-a) +
`440a46a` (A-5c-b host-bake).

## Public API (wire)

### USER_CREATE (verb 5) — grown OK response

USER_CREATE now mints the recovery keyslot as **mandatory enrollment** and
returns the phrase once. The OK payload grew append-only:

```
principal_id u32 LE | primary_gid u32 LE | phrase_len u16 LE | phrase
```

Pre-A-5c callers read the first 8 bytes; the only in-tree caller (joey's
corvus harness) was updated to accept the longer frame and verify the phrase
is present + self-consistent (`rlen == 10 + phrase_len`).

### RECOVER (verb 8) — new

```
Request (subject_kind = 1, user):
  subject_kind  u8  (1)
  user_len      u8  (1..=MAX_USER_LEN)
  user          user_len
  phrase_len    u16 LE (1..=RECOVERY_PHRASE_MAX = 215)
  phrase        phrase_len
  new_pass_len  u16 LE (1..=MAX_PASS_LEN)
  new_passphrase new_pass_len

OK reply:
  phrase_len    u16 LE
  fresh_phrase  phrase_len     (the rolled phrase; the used one is retired)
```

`subject_kind = 0` (system) is handled by `handle_recover_system` (A-5c-b; see
below) — it carries **no `user` field** (the subject is the fixed `b"system"`)
and adds a **console-attached** gate. For the user subject, **no session token
and no capability** are required — the user cannot AUTH (the passphrase is lost)
and a user-held recovery that needed the admin would not be user-held.

## Implementation

`usr/corvus/src/main.rs` + `usr/corvus/src/bip39_wordlist.rs`.

### BIP-39 codec

The phrase is the standard BIP-39 encoding of 256 bits of CSPRNG entropy: a
264-bit buffer `entropy(256) || checksum(8)` (the checksum is the first byte
of `SHA-256(entropy)`), split MSB-first into 24 11-bit indices into the
2048-word canonical English wordlist (`bip39_wordlist.rs`, sourced from
bitcoin/bips, SHA-256 `2f5eed…`; sorted, unique 4-char prefixes).

- `bip39_encode(&[u8;32]) -> Vec<u8>` — entropy → space-joined phrase.
- `bip39_decode(&[u8]) -> Option<[u8;32]>` — phrase → entropy; `None` on
  wrong word count, an unknown word, or a checksum mismatch. Splits on ASCII
  whitespace (runs collapsed); words are matched case-insensitively via
  binary search.

The KEK derives from the **decoded entropy**, not the phrase text, so
whitespace/case never affect key derivation (decode is canonical).

### The wrap

`recovery.corvus` uses the identical CRVS v1 on-disk layout as
`hybrid.corvus` (`TOTAL_LEN` = 3752 B; magic/version/argon2-params/salt/
nonce/ciphertext/tag), distinguished only by:

- **AD**: `"thylacine-corvus-recovery-v1" || subject || 0x00` —
  domain-separated from the passphrase-wrap AD prefix
  (`"thylacine-corvus-v1"`), so a recovery wrap can never be opened as a
  passphrase wrap or vice versa, even if a file is swapped.
- **KEK source**: `Argon2id(decoded_entropy, salt, recovery preset)` where
  the recovery preset is `t_cost = 8, m_cost = 16 MiB, p = 1` (the time cost
  is raised over the interactive `t_cost = 2`; the libsodium "sensitive"
  1 GiB `m_cost` is bounded out by the 24 MiB static heap — a v1.x
  heap-resize seam). The 256-bit phrase entropy is the security floor; the
  KDF cost is defense-in-depth, not the boundary.

`make_recovery_wrap` / `unwrap_recovery` are the seal/open;
`wrap_keypair_passphrase` is the shared passphrase-wrap path used by both
USER_CREATE's initial wrap and RECOVER's re-wrap (one code path keeps the AD
+ nonce discipline identical).

### The RECOVER(user) flow

1. Parse + bounds-check. The subject must exist (a nonexistent user fails
   `BadAuth` cheaply — no KDF, no rate-limit charge, no cost oracle).
2. **Rate-limit** (before any KDF): if the subject has `>= RECOVER_FAIL_MAX`
   (5) prior checksum-valid-but-unwrap-failed attempts this boot, return
   `RateLimited`.
3. Load `recovery.corvus` (absent → `BadAuth`; a user predating A-5c
   enrollment).
4. `bip39_decode` the phrase. A typo fails the checksum here — cheap, before
   the KDF, and **not** charged to the rate limit.
5. `unwrap_recovery` (the expensive, charged step). On tag mismatch
   (checksum-valid but wrong phrase = a crafted/guessed attempt) charge the
   rate limit and return `BadAuth`.
6. On success: build both new wraps in memory — re-wrap the keypair under the
   new passphrase (`hybrid.corvus`) and roll a fresh phrase
   (`recovery.corvus`) — then commit both via atomic rename-swap (hybrid
   first, then recovery; see "Crash safety"). Update the live user record's
   wrap fields so a subsequent AUTH works without a reboot. Reset the rate
   limit. Return the fresh phrase.

### Crash safety (the twin-wrap)

Both keyslots wrap the **same keypair**, so no crash ordering can strand the
user: whichever file survives independently recovers the keypair. RECOVER
commits `hybrid.corvus` first (the passphrase reset is the user's primary
goal) then `recovery.corvus`. A crash between them leaves the new passphrase
live **and** the old recovery phrase still valid (recovery.corvus untouched)
— the user can log in and re-run recovery. A fresh phrase is returned only
after it is durably on disk. Both swaps use `persist_wrap_swap` (write tmp →
fsync → rename → fsync dir), so a torn write never destroys the old file.

USER_CREATE enrollment writes `recovery.corvus` write-once (no existing file
to protect), alongside `hybrid.corvus`, both **before** the `identity.db`
commit — a crash leaves orphan wrap(s) (harmless; cleaned by the unlink-stale
step on a re-create), never an identity record pointing at a missing wrap.

### Rate limit (in-memory)

`RECOVER_FAILS` is a per-subject counter of checksum-valid-but-unwrap-failed
attempts, cleared on success. It bounds the unauthenticated `Argon2id` DoS
surface to 5 expensive attempts per subject per boot without a wall clock.
A legitimate holder's real phrase passes both the checksum and the unwrap on
the first try, so this never locks them out. The full time-windowed,
Stratum-persisted C-16 rate limit (covering AUTH too) is owed separately
(task #876).

## A-5c-b — the system identity (hostowner-c)

The **system** subject is the admin hybrid keypair, used by `ADMIN_ELEVATE` to
gate the `CAP_HOSTOWNER` grant. Unlike a user, the system identity has **no
`USER_STATES` record** and its wraps live at corvus's chroot **root** (not under
`users/<name>/`): `system-wrap` (the keypair under the system passphrase) and
`system-recovery-wrap` (the same keypair under a recovery phrase). Both are
**host-baked** at build time — they are the system analog of `pool.img` /
`system.key`.

### Host-bake (corvus-mint)

`tools/corvus-mint/` is a host-target tool that reuses corvus's exact crypto via
the shared `corvus-crypto` lib (no second implementation -> byte-identical
wraps). At build time `tools/build.sh` runs it during the pool populate:
generate the admin keypair, wrap it under the build-time system passphrase
(default `thylacine`, overridable via `CORVUS_SYSTEM_PASSPHRASE`) and under a
24-word recovery phrase, **self-verify both keyslots unwrap to the same
keypair**, and write the two CRVS-v1 blobs into the pool's `/var/lib/corvus`
(SYSTEM-owned), readback-verified byte-for-byte. The recovery phrase is logged
once (forensic, like the mkfs seed). At v1.0 the system passphrase is the known
constant so joey's boot `ADMIN_ELEVATE` stays green; the WRAP is real
Argon2id+AEGIS. A v1.x installer supplies a real per-install secret.

**Deterministic recovery phrase (A-5c-c).** The recovery-phrase *entropy* is
derived from `CORVUS_SYSTEM_RECOVERY_SEED` (a 64-hex seed; default a clearly
test value, overridable -- the same build-baked-known-secret posture as the
`thylacine` passphrase + the mkfs seed). corvus-mint has a second mode,
`emit-phrase <header>`, that derives ONLY the phrase from the seed and writes it
as a C header (`build/generated/corvus_system_recovery_phrase.h`), run by
`tools/build.sh` (`emit_corvus_recovery_header`) BEFORE the userspace build so
joey can `#include` it. Both the header and the pool bake derive from the same
seed, so the header phrase opens the baked `system-recovery-wrap` by
construction (no drift). This is what lets joey's boot harness drive a LIVE
`RECOVER(system)` (the keypair, salt, and nonce stay random per build; only the
phrase entropy is pinned -- joey needs the phrase, not the keypair). A v1.x
installer supplies real per-install randomness here too.

### `system_identity_load` (boot)

corvus reads `system-wrap` + `system-recovery-wrap` from its chrooted root
(post-pivot, so they resolve on the persistent Stratum pool where the bake
landed), parses them via `KeypairWrap::from_bytes` / `RecoveryWrap::from_bytes`,
and holds them in two statics (`SYSTEM_KEYPAIR_WRAP` / `SYSTEM_RECOVERY_WRAP` —
ciphertext only). **FATAL on absent/corrupt**: a missing wrap means a broken
bake or tampering, and corvus cannot authorize elevation without it — so it
refuses to start rather than fall back to a byte-compare. The boot banner line
is `corvus: system identity loaded (system-wrap + recovery)`. The wraps are
parse-validated only; the cryptographic check (does a passphrase open it?) is
deferred to `ADMIN_ELEVATE`, because corvus does not know the system passphrase.

### Real ADMIN_ELEVATE (verb 7)

The v1.0 placeholder byte-compared the supplied passphrase against a hardcoded
`SYSTEM_PASSPHRASE` constant. A-5c-b replaces that with
`unwrap_keypair_passphrase(SYSTEM_WRAP_SUBJECT, supplied, &SYSTEM_KEYPAIR_WRAP)`:
a correct passphrase opens the AEAD (the keypair is then wiped — ADMIN_ELEVATE
only needs the yes/no), any mismatch fails the tag -> `BadAuth`. The Argon2id
work is the deliberate brute-force cost on the system passphrase. The gate order
is unchanged (token -> console -> passphrase); only the passphrase step became
cryptographic. Fail-closed: an absent static (defensive — `system_identity_load`
is FATAL) or a KDF failure is `BadAuth`, never a bypass.

### RECOVER(system) — `handle_recover_system`

`RECOVER(subject_kind=0)` resets the system passphrase from the system recovery
phrase. Gate = **console-attached** (the EXTRA gate over RECOVER(user), mirroring
ADMIN_ELEVATE — the system identity is the most privileged secret, so it must
never be recoverable from a non-console peer) + the phrase + the rate limit; no
session token, no capability. The flow mirrors RECOVER(user) but operates on the
root-level system wraps + the statics instead of a `USER_STATES` record:

1. Console gate first (`PERMISSION_DENIED` if not console-attached), then parse
   (no `user` field).
2. Rate-limit (subject `b"system"`) before any KDF.
3. `unwrap_recovery(SYSTEM_WRAP_SUBJECT, decoded_phrase, &SYSTEM_RECOVERY_WRAP)`
   (the static borrow is scoped so it ends before the rolling writes). A typo
   fails the cheap checksum (not charged); a wrong phrase fails the tag (charged).
4. Re-wrap the recovered keypair under the new passphrase (rolling `system-wrap`)
   and roll a fresh recovery phrase (rolling `system-recovery-wrap`).
5. Commit `system-wrap` first (then update `SYSTEM_KEYPAIR_WRAP` so a same-boot
   ADMIN_ELEVATE uses the new passphrase), then `system-recovery-wrap` (then
   update `SYSTEM_RECOVERY_WRAP`). Same twin-wrap crash-safety as the user path:
   both wraps hold the same keypair, so a crash between leaves the new passphrase
   live AND the old phrase valid. The wraps live at the chroot root; `fd 0`
   (`STORAGE_ROOT_FD`) is the root dir handle on which `persist_wrap_swap`'s
   create/rename/fsync are all valid.
6. Reset the rate limit; return the fresh phrase.

## Data structures

- `RecoveryWrap` — `{ t_cost, m_cost_kib, parallelism, salt[16], nonce[32],
  ciphertext[3648], tag[32] }`, serialized to/from the CRVS v1 `TOTAL_LEN`
  layout. Pinned by the existing `_Static_assert`s on `TOTAL_LEN` /
  `KEYPAIR_LEN`.
- `KeypairWrap` — the in-memory result of `wrap_keypair_passphrase`
  (`{ salt, nonce, ciphertext, tag }`).
- `bip39_wordlist::BIP39_WORDLIST: [&str; 2048]`. Compile-asserted to fill
  the 11-bit index space exactly.

Compile-time invariants: `24 * 11 == 256 + 8`; the wordlist is exactly
`2^11`; the RECOVER payload and the phrase-bearing OK responses fit
`MAX_PAYLOAD_LEN` / `MAX_RESPONSE_FRAME`.

## Tests

- **`recovery_selftest()`** runs at every corvus boot (banner line
  `corvus: recovery self-test OK (bip39 + keyslot)`) — fast, no argon2:
  BIP-39 round-trip; a deterministic wrong-checksum reject; an unknown-word
  reject; the recovery-wrap byte round-trip; AD domain-separation (a foreign
  subject's AD **and** the passphrase-wrap AD both fail to open the wrap).
  corvus refuses to start (FATAL) on failure.
- **joey corvus harness** verifies USER_CREATE enrollment end-to-end on every
  boot: `USER_CREATE michael/susan ok (… recovery phrase enrolled)` with the
  phrase present + self-consistent.
- **`system_identity_load` + real ADMIN_ELEVATE** are proven on every boot: the
  banner `corvus: system identity loaded (system-wrap + recovery)` and
  `joey: ADMIN_ELEVATE ok` (joey supplies `thylacine` -> the real Argon2id+AEGIS
  unwrap of the host-baked `system-wrap` succeeds, then `t_cap_use(CAP_HOSTOWNER)`).
- **`corvus_crypto::tests::system_identity_lifecycle`** (host) proves the exact
  A-5c-b crypto sequence deterministically under `SYSTEM_WRAP_SUBJECT`: mint
  both keyslots over one keypair -> ADMIN_ELEVATE-unwrap -> RECOVER-unwrap ->
  re-wrap under a new passphrase (old passphrase no longer opens) -> roll a fresh
  phrase (old phrase no longer opens). 12/12 host tests.
- **Live `RECOVER(system)` boot E2E (A-5c-c, landed).** joey's console-attached
  corvus harness drives `RECOVER(system)` end-to-end on a fresh pool, with the
  seed-derived phrase from the generated header and `new_pass = "thylacine"` (so
  the passphrase is restored): boot log `joey: RECOVER(system) ok (live; system
  passphrase reset from recovery phrase; fresh phrase rolled)`. This is the
  first **live** execution of `handle_recover_system` (wire dispatch + console
  gate + argon2id+AEGIS unwrap of the baked wrap + persisted re-wrap). It is
  **fresh-pool-gated** (RECOVER rolls + persists the wrap, so a persistent pool
  must not re-run it) and **idempotent across reboots**: a second boot on the
  same pool skips it (`RECOVER(system) E2E skipped`) and `ADMIN_ELEVATE("thylacine")`
  still succeeds against the boot-1 re-wrapped `system-wrap` -- independently
  proving the re-wrap is a valid argon2id+AEGIS round-trip across a reboot.
- **Live `RECOVER(user)` via the login `!recover` UX (A-5c-c-2, landed).**
  `usr/joey/joey.c::do_recover_e2e` (fresh-pool-gated) captures michael's
  enrolled phrase from USER_CREATE, then spawns `/sbin/login` with a seeded pipe
  feeding `!recover\nmichael\n<phrase>\n<pass_michael>\n`. login's
  `do_recover_flow` drives a live `RECOVER(user)` (`login: recovery ok`); the new
  passphrase = the old one (RESTORE), so the subsequent `do_login_e2e`
  authenticates michael against the re-wrapped `hybrid.corvus` -- proving the live
  re-wrap is valid AND (since RECOVER re-wraps the keypair, not the DEK) the home
  DEK still unlocks. Idempotent across reboots (boot N+1 skips). The
  reset-takes-effect-with-a-CHANGED-passphrase property is host-proven (the
  `system_identity_lifecycle` round-trip asserts the old passphrase no longer
  opens after a reset).

## Error paths

| Status | Trigger |
|---|---|
| `BadFormat` | malformed payload; unknown `subject_kind` (not 0/1); over-long phrase/user/pass; `USER_CREATE` of the reserved name `system` (A-5c-c audit F1 -- the system AD subject) |
| `PermissionDenied` | RECOVER(system) from a non-console-attached peer; ADMIN_ELEVATE from a non-console peer |
| `BadAuth` | unknown user; absent `recovery.corvus`; bad checksum (typo); wrong phrase/passphrase (tag mismatch); ADMIN_ELEVATE wrong system passphrase |
| `RateLimited` | `>= RECOVER_FAIL_MAX` checksum-valid-but-wrong attempts for the subject this boot (user subject, or `b"system"`) |
| `InternalError` | RNG/KDF failure; FS open/persist failure (the passphrase may be reset but the phrase roll failed — old phrase still valid, no data loss); RECOVER(system) with a cleared system static (defensive — boot is FATAL on absent) |
| `OK` | passphrase reset; `phrase_len + fresh_phrase` returned |

## Status

**A-5c COMPLETE.** User subject (A-5c-a) + system subject / host-bake / real
ADMIN_ELEVATE (A-5c-b) + the login `!recover` UX, the live RECOVER(system) and
RECOVER(user) boot E2Es, and the build plumbing (A-5c-c) all landed. The whole
arc passed ONE focused adversarial audit (Opus prosecutor + self-audit): **CLEAN
0 P0 / 0 P1 / 1 P2 / 4 P3** -- architecture VERIFIED SOUND (I-22/I-2/I-6/I-27,
C-20/C-27/C-28, twin-wrap crash safety, secret hygiene on every path, no-drift
build). F1 [P2] (reserve the `system` username so a user cannot collide the
system AD subject) fixed + regression-tested; F2/F3 [P3] fixed (corvus-mint
phrase wipe; login `user_len` bound); F4 [P3] deferred to #876 (the persistent
rate-limit); F5 [P3] doc nit. See `memory/audit_a5c_closed_list.md`.

## Known caveats / deferred

- **RECOVER(system) live E2E** landed (A-5c-c-1): joey drives it on a fresh pool
  via the seed-derived phrase (the generated header). The focused adversarial
  audit of the whole A-5c surface is A-5c-c-3.
- **The v1.0 system recovery phrase is build-baked + deterministic** (seed
  `CORVUS_SYSTEM_RECOVERY_SEED`, default a known test value) — same posture as
  the known system passphrase. The phrase is logged at build time (forensic) and
  surfaced into joey via the generated header purely for the boot E2E; a v1.x
  installer supplies real per-install randomness (the `CORVUS_SYSTEM_RECOVERY_SEED`
  override is the seam). Overriding the seed without rebuilding joey makes the
  baked wrap and joey's header disagree -- the E2E then fails loudly (not silent).
- **The RECOVER(system) boot E2E consumes the baked phrase on the fresh boot**
  (it rolls `system-recovery-wrap` to an unknowable random phrase). Harmless at
  v1.0 (the system passphrase `thylacine` is known, so recovery is never needed;
  boots 2+ skip it) and consistent with the always-run boot harness that already
  creates the michael/susan test users + mutates the pool every boot. A v1.x
  production build that sets a real system passphrase MUST gate/strip the
  boot-test harness (the same requirement the baked passphrase + test users
  already impose) -- tracked at **#880**.
- **The v1.0 system passphrase is the known constant** (`thylacine`) — the WRAP
  is now real Argon2id+AEGIS, but the secret is build-baked, not a real
  per-install secret. A v1.x installer supplies that (the `corvus-mint`
  `CORVUS_SYSTEM_PASSPHRASE` override is the seam).
- **Rate limit** is in-memory only (resets on corvus restart, itself bounded
  by joey's restart-rate-limit). The persistent C-16 (AUTH + RECOVER,
  Stratum-backed) is task #876.
- **The fresh phrase rides the response buffer** to the caller (like the AUTH
  token); the staged-response Vec is not wiped after drain — a pre-existing
  pattern shared with the token, candidate for a global hardening pass.
- **The provisioning window**: USER_CREATE is admin-driven, so the
  provisioner transiently sees a user's *initial* phrase (the same trust
  window as the admin-set initial passphrase). It is not a standing escrow —
  it closes when the user runs RECOVER, which rolls a fresh phrase shown only
  to them. Forced first-login rotation is the near-term UX seam (A-5c-c).
