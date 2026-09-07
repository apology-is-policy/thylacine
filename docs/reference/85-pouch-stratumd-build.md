# 85 — pouch-stratumd-build: the first cross-compiled daemon [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-pouch-stratumd-build-doc-absorb`).
P6 sub-chunk 15: cross-compiling stratumd (Stratum's ~860 KiB static POSIX daemon)
for `aarch64-thylacine` against the pouch sysroot — proving the pouch
cross-toolchain end-to-end on a real multi-subsystem C codebase. **NOT
audit-bearing** (POUCH-DESIGN §14 row 15: Stratum-side coordination; pouch's
surface is unchanged). Its content lives, code-verified and current, in:

- the **build orchestration** — the `all -> kernel -> { ..., stratumd, ... }`
  dependency chain, the sysroot caching + the `sysroot_is_stale` lesson (a stale
  cached libc once shipped an 80-vs-72-byte buffer mismatch into stratumd — a
  silent stack overflow), and the `build_stratumd` cross-build step:

      vault/system/substrate/sub-substrate-build.md

- the **one Thylacine arm** — the `peer_creds.c` `__thylacine__` arm reusing the
  Linux `getsockopt(SO_PEERCRED)` body, which pouch's `0006-pouch-sockets` patch
  marshals onto `SYS_srv_peer` underneath (the "thin per-OS arm" POUCH-DESIGN §10
  measures a good port by):

      vault/system/boundary/pouch-seam/sub-pouch-net.md

- the **deployment** — joey spawning stratumd + the ramfs->`/sysroot` pivot (the
  audit-bearing follow-up, sub-chunk 16):

      vault/system/stratum/sub-stratum-boot.md
      vault/system/stratum/sub-stratum-session.md

- the **pouch-ld link driver rationale** — why the link routes through `pouch-ld`
  (clang mis-selects the host Darwin toolchain for unknown OS triples):

      vault/system/boundary/pouch-seam/sub-pouch-seam.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **Nothing load-bearing — a clean zero-fold of a build-recipe + coordination doc.**
  The substantive atoms (the SO_PEERCRED->SYS_srv_peer marshal, the build
  orchestration, the deployment, the pouch-ld rationale) are all home. The bulk of
  the doc — the `build_stratumd` CMake flags, the `STM_ENABLE_*=OFF` toggles, the
  10 platform-conditional Stratum files that fall through cleanly, the
  `thylacine-pouch-arm` branch coordination — is recipe and Stratum-side handoff,
  not a Thylacine invariant.
- **One UNOWNED build-recipe file noted, not folded.** `cmake/Toolchain-aarch64-pouch.cmake`
  (the `-D__thylacine__` / `-D_GNU_SOURCE` / `-nostdlibinc` / pouch-ld toolchain)
  is UNOWNED; its substantive rationale is in sub-pouch-seam, and it is a candidate
  for a broader pouch-build-substrate dossier if one is authored (one of the
  uncovered surfaces). The v1.0 `SO_PEERCRED`-returns-`uid=0`/`gid=0` lossiness (no
  uid model yet) is a known limitation, not a defect. Zero code change.
