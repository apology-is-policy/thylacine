# 76 — corvus ADMIN_ELEVATE + admin-verb gating [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-admin-elevate-doc-absorb`).
The userspace half of hostowner elevation: the corvus `ADMIN_ELEVATE` verb
(verb_id=7), joey's redemption (`t_cap_use`), and the C-22 admin-verb gating that
consumes the resulting `CAP_HOSTOWNER`. Its content lives, code-verified and
current, in:

- the **corvus consumer** — the `ADMIN_ELEVATE` verb (token -> live console
  re-query -> the *real* Argon2id + AEGIS system-passphrase unwrap -> `t_cap_grant`),
  the C-22 gating pattern (a fresh `t_srv_peer` per admin call, fail-closed on a
  dead or failed query — the snapshot deliberately does not authorize), and the
  first-user bootstrap exception (the one creation that needs no authority, cutting
  the hostowner chicken-and-egg cycle; a corrupt database aborts the boot rather
  than re-bootstrapping):

      vault/system/userspace/services/sub-corvus.md   (audit: hard)

- the **kernel elevation mechanism** — `SYS_CAP_GRANT` (32) / `SYS_CAP_USE` (33),
  `T_CAP_HOSTOWNER` (1<<3, elevation-only) / `T_CAP_GRANT_HOSTOWNER` (1<<4,
  fork-grantable joey->corvus), and the two-trust-domain defense-in-depth (corvus
  checks the passphrase; the kernel checks console-attached at `/cap/use`
  redemption):

      vault/system/kernel/security/sub-kernel-caps.md   (audit: hard)

- the **spec** — `corvus.tla`'s `AdminElevate`, `HostownerRequiresConsole`,
  `AdminRequiresProcCap`.

**What this file got WRONG or MISSED by the time it was absorbed:**

- **The doc is partially updated, and its stalest claim is superseded.** Sections
  48-50 and caveat 214 say `SYSTEM_PASSPHRASE` is the hardcoded byte string
  `"thylacine"` — but the doc's own line 69 already notes A-5c-b replaced the
  interim byte-compare with a real Argon2id + AEGIS unwrap of the host-baked
  `system-wrap`, and the current code carries no such hardcoded constant. The live
  mechanism (Argon2id + AEGIS, fail-closed on missing/corrupt/wrong passphrase) is
  the one `sub-corvus` documents. The "anyone with source-tree access can elevate"
  caveat died with the byte-compare.
- **The rest is current and home** — the C-22 fresh-live-query discipline, the
  bootstrap exception, the WRAP/USER_DELETE/ROTATE_KEY gates all riding the same
  `peer_live_caps & CAP_HOSTOWNER` pattern, and the RECOVER verb (A-5c) are carried
  by sub-corvus. The `t_open`-vs-direct-syscall note (the syscalls are the v1.0
  front end until a namespace-aware open lands) is a caps-surface detail. Zero code
  change.
