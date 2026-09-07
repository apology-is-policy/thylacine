# 69 — corvus WRAP/UNWRAP: ML-KEM-768 + X25519 hybrid keypair (P5-corvus-bringup-d) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-corvus-unwrap-absorb`).
The sub-chunk that made the keypair real (the hybrid ML-KEM-768 + X25519) and
added the DEK envelope with the WRAP/UNWRAP verbs. Its content splits by
code-owner:

- the **DEK envelope crypto** — the 1217-byte hybrid-PKE blob, the KEK bound to
  the ciphertext transcript, the associated-data binding to dataset and
  key-generation id (rotation-safe, non-replayable), and the fact that
  ML-KEM's **FIPS-203 implicit rejection** makes the **AEGIS-256 tag the sole
  integrity gate** of an unwrap — nothing upstream validates the ciphertext:

      vault/system/userspace/runtime/sub-corvus-crypto.md

- the **WRAP/UNWRAP verb wiring** — the C-7 ownership gate that fires *before*
  any crypto (the keypair is not copied out of the session slab until the
  caller is authorized), the keypair held in the mlock'd session slab and wiped
  per call, and the runtime getrandom-fatal posture:

      vault/system/userspace/services/sub-corvus.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- Its caveat "**USER_CREATE is not yet capability-gated**" is **stale** —
  USER_CREATE is now one of the capability-gated verbs (the daemon re-queries
  the peer's live caps at the call), per the dossier.
- The **provisioning path** it defers ("sealing a DEK for a not-yet-logged-in
  user ... lands with ADMIN_ELEVATE at P5-hostowner-b") has since landed; the
  recovery/elevation surface is described in the daemon dossier and in the
  absorbed `105-corvus-recovery`.
- The kernel-side `EXEC_USER_STACK_SIZE` 16 KiB -> 256 KiB bump it records is an
  **exec-loader** detail (ML-KEM keygen/decapsulate are stack-heavy), not
  corvus's — its as-built home is the exec surface, not this file. The
  user-stack guard page it notes as audit-F7 lives there too.
