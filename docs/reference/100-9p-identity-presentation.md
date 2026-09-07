# 100 — 9P identity presentation (A-3) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-9p-identity-absorb`).
A-3 reconciles a 9P server's notion of *who is connecting* with Thylacine's
durable per-Proc `principal_id`, so the kernel's rwx enforcement can be
activated against `dev9p` without bricking boot — and corrects the F-4 design:
the local identity channel is **`SO_PEERCRED`**, not the 9P `n_uname` field.
Its content spans several code-owners:

- the **`SO_PEERCRED`-carries-principal marshal** (M1) — the pouch shim marshals
  the kernel-stamped `principal_id -> ucred.uid` + `primary_gid -> ucred.gid`
  (was a `0/0` stub), the unforgeable local identity channel:

      vault/system/boundary/pouch-seam/sub-pouch-net.md

- **whose identity the Stratum server believes** — `n_uname` ignored,
  `SO_PEERCRED` the live channel, plus the **host-bake owner override** (M2:
  `--bake-owner-uid` / `-gid` overriding `auth_uid` / `auth_gid` at the create
  chokepoint, `bake_owner_enabled` default-false, `(uid_t)-1` sentinel):

      vault/system/stratum/sub-stratum-server.md

- **the bake-value pass** (M2 completion) — `build.sh` stamping `PRINCIPAL_SYSTEM`
  (`4294967294`) via `stratum-mkfs --root-uid` (the pool ROOT inode) + host
  `stratumd --bake-owner-uid` (every baked file), the no-brick property; plus
  `stratum_host_tools_stale`:

      vault/system/substrate/sub-substrate-build.md

- **the syscall-path A-3 touches** — M4 (`n_uname = principal` at the attach
  handlers), F2 (rename/unlink `perm_check(PERM_W|PERM_X)` gated on
  `perm_enforced`), the walk-open handle-rights caller policy
  (`rights_for_omode | RIGHT_TRANSFER`; `T_OPATH` born `R|W` no-transfer), and
  A-3c's `attach_err_to_ret` ({-1,fd} -> {-errno,fd}, surfacing the Tattach
  `-EACCES`):

      vault/system/kernel/entry/sub-kernel-syscall-dispatch.md

- **the identity axis + the F1 `rights_for_omode` map** — `perm_check`,
  `perm_want_for_omode` / `rights_for_omode` as a matched pair, the OEXEC
  execute-to-read leak, and the `perm_enforced` seam:

      vault/system/kernel/security/sub-kernel-perm.md

- **the A-3c ecode surfacing on the client** — `map_error` bounding the wire
  ecode to `[1,4095]` before negation (the F1 signed-overflow fix), and
  `p9_attached_create`'s `out_err` threading the Tattach `-T_E_ACCES` out:

      vault/system/kernel/ninep/sub-kernel-ninep-client.md
      vault/system/kernel/ninep/sub-kernel-ninep-attach.md

- **the enforcement activation** — `dev9p.perm_enforced = true`, and the
  fail-closed uid/gid stat that makes it coherent:

      vault/system/kernel/ninep/sub-kernel-ninep-dev9p.md

- **the per-user-stratumd dataset-scope refusal** (M6) — the `--datasets-allowed`
  proxy gate that emits `Rlerror(EACCES)`:

      vault/system/stratum/sub-stratum-session.md

- **the M5 trust-stamp gate** — recorded as a v1.x seam (no v1.0 caller; not
  built): gating the `n_uname` assertion on a corvus trust bit before asserting
  identity to a server whose peer the kernel does not stamp:

      vault/seams/seam-nuname-trust-stamp.md

- **the I-22 property** it preserves (the presented principal is kernel-stamped,
  never client-asserted; no ambient root):

      vault/invariants/inv-i22.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- Its account of the client ecode path was, at absorption, **contradicted by the
  code**: the client's `map_error` bounds the wire ecode to `[1,4095]` before
  negating (closing the `-(int)0x80000000` signed-overflow UB — a kernel halt
  reachable by any hostile `Rlerror`). The `sub-kernel-ninep-client` dossier had
  said the opposite ("verbatim / u32-unbounded here / bounded at dev9p, not
  here"); the absorption rewrote those lines to match the code and the sibling
  wire dossier.
- The `n_uname` trust-stamp gate (M5) was recorded here as a seam but was
  **un-homed** in the vault graph — the attach dossier's "swept there" pointer
  named no node. It is now `seam-nuname-trust-stamp`.
- It cites `include/stratum/inode.h:202-203` (the `si_uid`/`si_gid` fields) — a
  foreign Stratum header not in this checkout; the fact (no format bump, only
  the stamped value differs) is carried by the two Stratum dossiers.
- `stratum-mkfs --root-uid`/`-gid` is a foreign Stratum tool; its *use* (the
  no-brick bake value) is homed in `sub-substrate-build`, not a mkfs dossier.
