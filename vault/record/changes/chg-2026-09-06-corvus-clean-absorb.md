---
id: chg-2026-09-06-corvus-clean-absorb
type: chg
title: "docs/reference retirement: absorb the corvus CLEAN set -- 65-skeleton + 66-server + 68-crypto (split) + 74-9p-server (dual); fold 74's 4 verified minor atoms into sub-corvus (message-oriented /ctl, accept-time fail-closed peer read, conn-id zero-skip, no-Tflush); no security gaps in this set (60 absorbed / 97 live)"
date: 2026-09-06
arc: arc-vault
commits: ["b24297b0"]
touched: [sub-corvus]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
The corvus area's four no-security-gap files, absorbed together (the
security-critical folds -- 69/97/105 -- follow in their own chgs, one per file,
for auditability). Owners split cleanly by CODE: sub-corvus owns the daemon
(`usr/corvus/src/main.rs` -- server, 9P, verbs, session, identity-db, recovery
flow); sub-corvus-crypto owns the primitives (`usr/lib/corvus-crypto` --
Argon2id, AEGIS-256, the CRVS wrap, the DEK envelope, BIP-39). The
OWNERSHIP-SPLIT trap the plan warned of held: docs titled "crypto" (68) split,
they do not stub wholesale to the crypto dossier.

- **65-corvus-skeleton** -> sub-corvus (single). CLEAN. The startup-hardening
  sequence + joey's cap-grant are covered by the dossier's Mechanism. Its
  set_traceable "no debug subsystem to gate" caveat is now STALE (I-39 debug-fs
  landed + enforces NOTRACE); noted in the stub. No fold.
- **66-corvus-server** -> sub-corvus (single). CLEAN. Wire format + session +
  server loop covered; the fd 0/1 pipe transport it documents is RETIRED
  (superseded by 9P), noted. No fold.
- **68-corvus-crypto** -> sub-corvus-crypto (crypto) + sub-corvus (verb wiring).
  SPLIT-redirect, no absent security atom (both the KDF/AEAD/wrap and the
  USER_CREATE/AUTH flow are already carried). No fold.
- **74-corvus-9p-server** -> sub-corvus + sub-kernel-srvconn (dual). Near-clean;
  FOUR minor atoms folded into sub-corvus, each VERIFIED against main.rs (the
  Explore flagged 3; verify-before-stub found the 4th): (1) /ctl is
  message-oriented -- Tread ignores the client offset, corvus drains from its
  own pending_response_off (main.rs:3987-3989); (2) the accept-time t_srv_peer
  read is itself fail-closed -- a failed read closes the handle rather than
  admitting a zero-identity Conn, so failure cannot alias the fail-closed
  zero-cap value (main.rs:4180-4205, F4); (3) the monotonic conn-id skips 0 on
  the 64-bit wrap so a recycled id cannot alias the "no owner" sentinel and pass
  the SESSION_CLOSE ownership gate (main.rs:4198-4204); (4) no Tflush handler --
  the kernel client is single-flight (Seams). corvus's own p9.rs is the codec
  ANCESTOR the shared runtime codec was lifted from (already in the dossier).

Effort-gate: max (confirmed) -- corvus is audit:hard crypto/key-agent, so every
folded atom was verified in-tree before it entered a dossier. No code touched;
no audit owed (doc absorption). sub-corvus updated: 2026-08-16 -> 2026-09-06.
view-absorption: 56 -> 60 absorbed, 97 live.
