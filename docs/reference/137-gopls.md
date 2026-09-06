# 137 — gopls (the Go LSP engine) on Thylacine [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-gopls-doc-absorb`).
The `GOOS=thylacine` userspace port of gopls (the Go language server, NOVEL #13),
Stage 8d. **No kernel surface** — gopls is pure Go; the only Thylacine-tree code is
a joey boot probe + a `t_chdir` wrapper. The engine fork itself lives in an
external repo (`~/projects/gopls`, base v0.21.1: two build-fallback shims +
telemetry disabled). Its Thylacine-side content lives, code-verified and current,
in:

- the **#99 filecache open-or-create finding** (RESOLVED) — the non-atomic
  `SYS_OPEN`-then-`SYS_WALK_CREATE` race under gopls's concurrent content-addressed
  `Set`s: `dev9p_create` now records the real errno (so EL0 sees `EEXIST` not a
  bare `-1`->EPERM) AND drops the `(parent,name)` dentry + bumps the gen on
  create-EEXIST, so a loser's retry-`Open` sees the file rather than a stale
  negative dentry:

      vault/system/kernel/ninep/sub-kernel-ninep-dev9p.md   (audit: hard, FS-mutation)

- the **#100 robustio FileID devno finding** (RESOLVED) — `t_stat` grew 80->88
  with a `devno` field (Plan 9 `Chan.dev` / POSIX `st_dev`), so the shim returns
  `FileID{device: stat.Dev, inode: stat.QidPath}` and a cross-dataset qid.path
  collision can no longer serve file A's bytes under file B's URI (the fail-
  dangerous `device: 0` hazard):

      vault/system/boundary/registries/abi-t-stat.md   (88 bytes)

- the **env-requirement chain** — gopls needs `CAP_CSPRNG_READ` (crypto/rand's
  trace-ID seed is fatal without it), `PATH` (to resolve `go`), and a module cwd;
  login stamps `SHELL_CAPS` + seeds `PATH`:

      vault/system/stratum/sub-stratum-session.md
      vault/system/kernel/security/sub-kernel-caps.md

- the **boot probe** (`joey: go8d OK`) and the **LSP client** it will drive (Nora,
  Stage 8e):

      vault/system/kernel/boot/sub-kernel-joey.md
      vault/system/userspace/shell-tui/sub-parley.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **Nothing load-bearing — a clean zero-fold of a port doc.** The two RESOLVED
  Thylacine-tree findings (#99 the dev9p create-EEXIST errno + dentry-drop, #100
  the `t_stat` devno ABI) are home in the kernel and ABI dossiers; the env chain is
  the caps + login's; the LSP client is sub-parley's (Stage 8e). The gopls fork
  itself (the two `//go:build unix`-gap shims, telemetry disabled, the teardown-segv
  #98 disposition — which did not reproduce under a full-env boot) is external, in
  `~/projects/gopls`, not a Thylacine-tree surface. A build-time lesson worth
  keeping — a per-mirror `_Static_assert(sizeof == 80)` checks only its own size,
  not that it matches the kernel, so a stale mirror overflows at runtime (patches
  0019/0021, caught by a boot segv, not the build) — is carried with the `t_stat`
  ABI. Zero code change.
