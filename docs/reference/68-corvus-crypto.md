# 68 — corvus crypto: Argon2id + AEGIS-256 + state file (P5-corvus-bringup-c) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-corvus-clean-absorb`).
The chunk that made AUTH real crypto. Its content splits by code-owner — the
primitives and the wrap layout belong to the crypto crate, the verb flow to the
daemon:

- the **crypto core** — the Argon2id key-derivation, the AEGIS-256 AEAD, the
  `CRVS` wrap layout (72-byte header: magic, version, the three Argon2 costs,
  salt, nonce; then ciphertext and a 32-byte tag), the associated-data domain
  separation built through named helpers, the compile-time layout asserts, and
  the 16 MiB-vs-64 MiB cost story bounded by the fixed heap:

      vault/system/userspace/runtime/sub-corvus-crypto.md

- the **verb wiring** — USER_CREATE minting a wrapped `CorvusUserState`, AUTH
  re-deriving the KEK and AEGIS-unwrapping to a tag-checked success, and the
  wipe-on-every-path discipline through the AUTH failure legs:

      vault/system/userspace/services/sub-corvus.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- The **keypair it wraps is a placeholder.** This chunk AEGIS-wrapped a 64-byte
  CSPRNG blob where the identity keypair belongs; the real ML-KEM-768 + X25519
  hybrid keypair (3648 bytes) landed the next sub-chunk (see `69-corvus-unwrap`,
  itself absorbed). `KEYPAIR_LEN` is the only constant that moved; the crypto
  dossier describes the real keypair and the DEK envelope that consumes it.
