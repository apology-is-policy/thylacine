---
id: chg-2026-09-06-corvus-unwrap-absorb
type: chg
title: "docs/reference retirement: absorb 69-corvus-unwrap -- fold the 2 SECURITY crypto atoms into sub-corvus-crypto (FIPS-203 implicit-rejection => AEGIS-256 tag is the SOLE integrity gate; dataset+key_id AAD binding => rotation-safe/non-replayable) + 2 daemon minors into sub-corvus (C-7 gate before any crypto; runtime getrandom-fatal); dual-redirect stub (61 absorbed / 96 live)"
date: 2026-09-06
arc: arc-vault
commits: []
touched: [sub-corvus, sub-corvus-crypto]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
The first security-critical corvus fold. Each atom was VERIFIED against the code
before it entered a dossier (corvus is audit:hard crypto/key-agent; effort max).

Into sub-corvus-crypto (the primitives, `usr/lib/corvus-crypto`):
- **FIPS-203 implicit-rejection => the AEGIS-256 tag is the SOLE integrity gate
  for a DEK unwrap.** Verified at `dek_envelope_unwrap` (lib.rs:378): ML-KEM
  `decapsulate` never rejects a length-valid ciphertext -- it returns a
  deterministic-but-wrong shared secret -- so a tampered ciphertext derives a
  wrong KEK and the ONLY place it is caught is `aegis_unwrap`'s tag check
  (lib.rs:401). Nothing upstream validates the ciphertext. The dossier's
  envelope Mechanism previously described the KEK transcript binding but not
  this consequence.
- **dataset + key_id bound into the AEAD associated data => rotation-safe,
  non-replayable.** Verified at lib.rs:339-341 (wrap) and :398-400 (unwrap):
  AD = DEK_AD_PREFIX ++ ad_dataset ++ key_id.to_le_bytes(). An envelope wrapped
  under one (dataset, key-gen) fails the tag if presented under another. The
  dossier named the transcript binding but not this AD binding.

Into sub-corvus (the daemon, `usr/corvus/src/main.rs`):
- **C-7 owner gate fires before any crypto.** Verified in `handle_unwrap`: token
  -> session -> `dataset_owner_find` -> owner-vs-session check (PermissionDenied)
  ALL precede `session_keypair_copy()` + `dek_envelope_unwrap`. The keypair is
  not copied out of the mlock'd slab until the caller is authorized -- an
  unauthorized unwrap costs nothing and never brings key material onto the path.
- **Runtime getrandom-fatal.** Verified at `ThylaRng::fill_bytes` (main.rs:253-263):
  a mid-operation `t_getrandom` rc<=0 calls `t_exits(1)` -- the daemon dies
  rather than draw a token/salt/nonce from a generator that stopped answering.
  corvus proved the CSPRNG at boot, so a later failure is an invariant violation.

Stub: dual-redirect (crypto -> sub-corvus-crypto, verb wiring -> sub-corvus).
Honest "what it got wrong": USER_CREATE is now capability-gated (its caveat is
stale); the provisioning path it defers has landed; the EXEC_USER_STACK_SIZE
16->256 KiB bump is an exec-loader detail, not corvus's. No code touched; no
audit owed. sub-corvus-crypto updated: 2026-08-04 -> 2026-09-06 (sub-corvus
already at 2026-09-06). view-absorption: 60 -> 61 absorbed, 96 live.
