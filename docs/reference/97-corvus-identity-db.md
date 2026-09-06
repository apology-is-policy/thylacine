# 97 — corvus identity DB + the first secret-on-disk path (A-1b) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-corvus-identity-db-absorb`).
corvus as the `id <-> name <-> groups` authority with real on-disk persistence.
The corvus-side content lives in the daemon dossier:

- the **identity resolver + persistence** — the `identity.db` (non-secret map)
  and per-user keypair wrap (ciphertext), the **secret boundary** between them
  (no plaintext secret ever reaches the FS, C-24), the atomic rewrite-swap with
  the **wrap durable before the identity record** (a crash leaves a harmless
  orphan wrap, never a dangling record) and the fail-closed drop of a
  missing/corrupt wrap at load, and the **I-22 id allocation** — one monotonic
  counter that refuses the reserved range and is persisted so a freed id is
  never re-minted, with the user-private-group `uid == gid` scheme:

      vault/system/userspace/services/sub-corvus.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- Much of it is **not corvus's to own.** The FS-mutation syscalls it leans on
  (`SYS_WALK_CREATE` / `FSYNC` / `READDIR` / `RENAME` / `UNLINK`) are the
  kernel's surface; the storage-capability confinement is the T_OPATH / chroot
  surface; and the long "History — the masking stack behind the AEGIS-256
  corruption" is a **debugging journal**, not a subsystem reference — its home is
  `docs/DEBUGGING-PLAYBOOK.md` section 6.10, which it already cites. The
  load-bearing cross-reboot fixes were **Stratum-side** (the bdev partial-tail
  RMW + read-offset alignment), not corvus's.
- Its "durability note" that the per-directory fsyncs are forward-portable
  insurance (whole-pool `Tfsync` on current Stratum) is a Stratum-contract
  observation, correct but not a corvus mechanism — kept in the dossier only as
  the "do not optimize away" prosecution point.
