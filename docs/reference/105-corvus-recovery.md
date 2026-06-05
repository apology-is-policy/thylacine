# 105 — corvus recovery keyslot (A-5c-a)

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
keyslot) and the `RECOVER(subject_kind=1)` verb. The **system** subject
(`system-recovery-wrap`, = P5-hostowner-c) is A-5c-b.

Design: `CORVUS-DESIGN.md` §5.6 + §8 + §9 (C-20/C-27/C-28);
`IDENTITY-DESIGN.md` §9.9. Scripture-first commit `064807c`.

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

`subject_kind = 0` (system) is rejected with `BadFormat` at A-5c-a (A-5c-b
adds it). **No session token and no capability** are required — the user
cannot AUTH (the passphrase is lost) and a user-held recovery that needed the
admin would not be user-held.

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
- The full argon2-backed RECOVER round-trip (create → capture phrase → reset
  passphrase → re-login → home decrypts) is the **A-5c-c** boot E2E.

## Error paths

| Status | Trigger |
|---|---|
| `BadFormat` | malformed payload; `subject_kind = 0` (A-5c-b); over-long phrase/user/pass |
| `BadAuth` | unknown user; absent `recovery.corvus`; bad checksum (typo); wrong phrase (tag mismatch) |
| `RateLimited` | `>= RECOVER_FAIL_MAX` checksum-valid-but-wrong attempts for the subject this boot |
| `InternalError` | RNG/KDF failure; FS open/persist failure (the passphrase may be reset but the phrase roll failed — old phrase still valid, no data loss) |
| `OK` | passphrase reset; `phrase_len + fresh_phrase` returned |

## Status

Landed at A-5c-a. The user subject is complete; the system subject
(hostowner-c) is A-5c-b; the login UX + boot E2E + the focused audit are
A-5c-c.

## Known caveats / deferred

- **System subject** (`subject_kind = 0`) returns `BadFormat` until A-5c-b,
  which must first build the real `system-wrap` (the v1.0 system passphrase
  is still the `ADMIN_ELEVATE` byte-compare placeholder).
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
