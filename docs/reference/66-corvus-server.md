# 66 — corvus server loop + wire codec (P5-corvus-bringup-b) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-corvus-clean-absorb`).
This chunk added the verb wire format, the session table and the server loop.
The wire contract and the session lifecycle are current and live in the daemon
dossier; the *transport* this file documents is not.

- the **wire format** (the 4-byte request header, the 3-byte response header,
  the seven statuses, the deliberate BadAuth/NotFound merge so the wire does not
  enumerate accounts), the **session** (one user, one 33-byte opaque token, one
  keypair; installed and cleared whole so its identity is unexpressible-to-mutate
  rather than checked), the **server loop** and the per-frame secret wipe, and
  the **one-session-at-a-time AUTH gate**:

      vault/system/userspace/services/sub-corvus.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- The **transport it documents is retired.** This chunk ran a single peer over a
  kernel pipe pair (corvus's fd 0 + fd 1); corvus is now a real 9P2000.L server
  reached via `/srv/corvus`, and its handle table at startup is empty — no fd 0,
  no fd 1. The file's own Status section records the supersession
  (P5-corvus-srv-impl-b3b); the dossier describes the 9P transport as the only
  one.
- AUTH here "accepts any non-empty passphrase" — a skeleton before the crypto
  gate. The real Argon2id + AEGIS-256 gate landed the next sub-chunk; the
  dossier and the corvus-crypto dossier describe the enforced path.
