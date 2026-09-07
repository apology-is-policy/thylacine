---
id: chg-2026-09-06-corvus-recovery-absorb
type: chg
title: "docs/reference retirement: absorb 105-corvus-recovery (the heaviest) -- author sub-corvus-mint (the 3rd orphan: tools/corvus-mint, host system-identity minter) + fold 4 SECURITY atoms into sub-corvus (RECOVER(user) unauthenticated; ADMIN_ELEVATE real crypto not byte-compare; twin-wrap crash-safety; bounded provisioning window); triple-redirect stub (63 absorbed / 94 live, +1 new dossier sub-corvus-mint)"
date: 2026-09-06
arc: arc-vault
commits: ["e9810d3f"]
touched: [sub-corvus]
established: [sub-corvus-mint]
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
The heaviest, last corvus absorption. Two deliverables: a NEW dossier for the
3rd orphan, and the 4 security folds. Each atom VERIFIED against
usr/corvus/src/main.rs before entering a dossier (audit:hard; effort max).

**Authored sub-corvus-mint** (parent moc-substrate, audit:hard, code:
tools/corvus-mint/src/main.rs + Cargo.toml). The host-target minter that writes
the system identity (system-wrap + system-recovery-wrap of one keypair) the
device's corvus must open at boot -- the second byte-identical-wrap binary
sub-corvus-crypto exists for. audit:hard is a deliberate exception in the
otherwise-audit:none substrate area: it is not a harness but a SECRET PRODUCER,
and the secret is the most privileged one in the system. Its self-verify
(unwrap both keyslots, assert == the keypair, before baking) is the area's
"verify the artifact not the intent" applied to a secret. Cross-links
sub-corvus-crypto (crate) + sub-corvus (consumer) + sub-substrate-build
(build-from-root + when build.sh runs it). This resolves the 3rd orphan
(alongside uart.c/joey.c) so 105 can fully stub.

**Folded into sub-corvus** (the daemon recovery), all verified:
- RECOVER(user) takes NO token + NO cap (handle_recover subject_kind=1,
  main.rs:2357+ -- no session_token_matches, no cap gate; the phrase + BIP-39
  checksum + per-subject rate limit are the whole gate). RECOVER(system) +
  ADMIN_ELEVATE add a live-console gate.
- ADMIN_ELEVATE is a REAL Argon2id+AEGIS unwrap (main.rs:3276
  unwrap_keypair_passphrase), NOT a byte-compare (retired); the keypair is
  wiped at once (3277, yes/no only); gate order token(3247)->console(3257)->
  passphrase(3276); fail-closed to BadAuth.
- twin-wrap crash-safety: handle_recover commits persist_wrap_swap(HYBRID)
  BEFORE persist_wrap_swap(RECOVERY_FILE); both hold the same keypair so a crash
  between leaves the new passphrase live AND the old phrase valid.
- bounded provisioning window: USER_CREATE returns the initial phrase once
  (main.rs:2323-2335, then wipes it); only the ciphertext wrap persists -- no
  standing escrow; RECOVER rolls a fresh phrase shown only to the user.

Crypto side (BIP-39, AD domain-sep, t_cost=8 preset) already in
sub-corvus-crypto -- no crypto fold owed. Triple-redirect stub (daemon ->
sub-corvus, crypto -> sub-corvus-crypto, host-bake -> sub-corvus-mint); the
login !recover UX + boot E2Es + build plumbing are noted as login/joey/build
territory, not corvus's. No code touched; no audit owed (doc absorption + a new
dossier over existing code). view-absorption: 62 -> 63 absorbed, 94 live (157 total; the new sub-corvus-mint
is a dossier, not a legacy doc, so it does not change the legacy denominator).
The corvus reference set is now FULLY absorbed.
