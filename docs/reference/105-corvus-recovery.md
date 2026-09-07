# 105 — corvus recovery keyslot (A-5c-a + A-5c-b + A-5c-c) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-corvus-recovery-absorb`).
The recovery keyslot: a second wrap of a subject's keypair under a BIP-39
phrase, plus the system-identity host-bake and the real `ADMIN_ELEVATE`. Its
content spans three code-owners:

- the **daemon recovery flow** — `RECOVER(user)` taking **no token and no
  capability** (the phrase, the BIP-39 checksum, and a per-subject rate limit
  are the whole gate), `RECOVER(system)` and `ADMIN_ELEVATE` adding a live
  console gate, `ADMIN_ELEVATE` as a **real** Argon2id+AEGIS unwrap of the
  system wrap (the keypair wiped, the v1.0 byte-compare retired), the
  **twin-wrap crash-safety** (the passphrase wrap commits before the recovery
  wrap; both hold the same keypair), and the bounded **provisioning window**
  (the admin sees a user's initial phrase once, never a standing escrow):

      vault/system/userspace/services/sub-corvus.md

- the **recovery crypto primitives** — the BIP-39 codec (entropy, not phrase
  text, drives the KEK), the recovery wrap's AD domain separation from the
  passphrase wrap, and the raised `t_cost = 8` recovery Argon2id preset:

      vault/system/userspace/runtime/sub-corvus-crypto.md

- the **host-bake** — `corvus-mint`, which writes `system-wrap` +
  `system-recovery-wrap` at build time, self-verifies both open the same
  keypair, and emits the deterministic recovery phrase as a header for joey:

      vault/system/substrate/sub-corvus-mint.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- Much of A-5c-c is **not corvus's**: the login `!recover` UX flow
  (`do_recover_flow`) is the login program's, the boot E2Es are joey's
  harness, and the build plumbing (`emit_corvus_recovery_header`, the header
  include) is `tools/build.sh` — the dossiers above own the corvus/crypto/mint
  pieces, not the login/harness/build glue.
- Its "no kernel and no Stratum surface" claim is correct and load-bearing
  (recovery re-wraps the keypair, not the DEK, so every envelope stays valid) —
  kept in the daemon dossier as the reason the model works.
- The deferred items it lists (the persistent C-16 rate limit #876, the
  boot-harness gating #880, the response-buffer wipe hardening) are real open
  seams, carried on the dossiers rather than in this frozen file.
