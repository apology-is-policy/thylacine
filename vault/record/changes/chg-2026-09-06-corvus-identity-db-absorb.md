---
id: chg-2026-09-06-corvus-identity-db-absorb
type: chg
title: "docs/reference retirement: absorb 97-corvus-identity-db -- fold 2 SECURITY atoms into sub-corvus (identity.db is NON-secret vs the ciphertext wrap = the C-24 boundary; UPG shared monotonic id-alloc refuses >= PRINCIPAL_SYSTEM = I-22, persisted/never-reused) + 2 minors (wrap-before-record persist ordering; dropped-wrap fail-closed on load); single-redirect stub (62 absorbed / 95 live)"
date: 2026-09-06
arc: arc-vault
commits: ["d8b08570"]
touched: [sub-corvus]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
The second security-critical corvus fold. Each atom VERIFIED against
usr/corvus/src/main.rs before entering the dossier (audit:hard; effort max).

Folded into sub-corvus:
- **The on-disk split is a secret boundary (C-24).** identity.db is the
  non-secret uid<->name<->gid map (/etc/passwd-shaped); the keypair wrap is the
  only secret and it is ciphertext. Verified `identity_db_serialize` writes ONLY
  principal_id/primary_gid/supp_gids/name/gid -- no salt/nonce/ct/tag/keypair
  bytes -- so no plaintext secret reaches the FS.
- **I-22 id allocation.** `alloc_auto_id` (main.rs:414-422) refuses
  `< FIRST_AUTO_ID(1000)` or `>= PRINCIPAL_SYSTEM(0xFFFF_FFFE)`; the single `>=`
  covers BOTH reserved sentinels because const-asserts (401-402) pin
  PRINCIPAL_NONE > PRINCIPAL_SYSTEM > FIRST_AUTO_ID. NEXT_AUTO_ID is monotonic
  and persisted in the db header (761), so a freed id is never re-minted; the
  UPG gid comes from the same counter (uid==gid collision-free).
- **Persist ordering (minor).** handle_user_create (main.rs:2267-2292):
  persist_keypair_wrap (+ recovery wrap) durable FIRST, abort-before-identity.db
  on failure; THEN the in-memory append + identity_persist rewrite-swap with
  rollback. A crash between leaves a harmless orphan wrap, never a record
  pointing at a missing secret.
- **Dropped-wrap fail-closed (minor).** load (main.rs:1122-1129): a user whose
  hybrid.corvus is missing/corrupt is dropped fail-closed (logged, not
  authoritative), not admitted without a usable secret.

Single-redirect stub -> sub-corvus. Honest "what it got wrong": much of the doc
is NOT corvus's -- the FS-mutation syscalls are the kernel's, the "AEGIS-256
corruption" masking-stack history is DEBUGGING-PLAYBOOK material (+ the
load-bearing fixes were Stratum-side), the whole-pool-Tfsync note is a Stratum
contract. No code touched; no audit owed. sub-corvus already at 2026-09-06.
view-absorption: 61 -> 62 absorbed, 95 live.
