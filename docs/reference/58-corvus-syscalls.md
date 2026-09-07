# 58 — Corvus hardening syscalls (P5-corvus-syscalls) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-corvus-syscalls-doc-absorb`).
Five v1.0 hardening syscalls (`SYS_MLOCKALL` 16, `SYS_SET_DUMPABLE` 17,
`SYS_SET_TRACEABLE` 18, `SYS_EXPLICIT_BZERO` 19, `SYS_GETRANDOM` 20) + two caps
(`CAP_LOCK_PAGES`, `CAP_CSPRNG_READ`) + the one-way `PROC_FLAG_NODUMP/NOTRACE/
MLOCKED`, the startup scaffold corvus and per-user stratumd consume. Its content
lives, code-verified and current, in:

- the **CSPRNG behind `SYS_GETRANDOM`** — `kern_random_bytes`, the ChaCha20 stir,
  RNDR/FEAT_RNG, and the DTB-seed + CNTPCT-jitter mixing:

      vault/system/kernel/devices/sub-kernel-content.md   (audit: hard)

- the **`PROC_FLAG_NOTRACE` enforcement** — the I-39 debug gate refuses a NOTRACE
  target (the flag the doc calls "not enforced at v1.0" now gates):

      vault/system/kernel/introspection/sub-kernel-devproc.md

- the **one-way `proc_flags` mechanism** — `proc_mark_*` / `proc_is_*`, the
  monotonic stamps and their fail-closed readers that carry NODUMP/NOTRACE/MLOCKED:

      vault/system/kernel/execution/sub-kernel-proc.md

- the **caps + corvus's hardening posture** — `CAP_LOCK_PAGES` / `CAP_CSPRNG_READ`
  in the registry, and corvus's "mlockall'ed, undumpable, untraceable" startup:

      vault/system/boundary/registries/abi-caps.md
      vault/system/userspace/services/sub-corvus.md

- the **`explicit_bzero` secret-wipe** in its crypto context:

      vault/system/userspace/runtime/sub-corvus-crypto.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **Two framings are stale, not merely aged — both superseded by landed work.**
  (1) The doc says `SYS_GETRANDOM` "returns -1 immediately... no software-CSPRNG
  mixing means RNDR-absent is permanent" — **refuted**: the Lazarus W3 ChaCha20
  stir landed, so RNDR is no longer the sole source and an RNDR-less target (Apple
  under HVF, the A72) seeds from the DTB boot seed + CNTPCT jitter (sub-kernel-
  content). (2) "Flags are not enforced at v1.0" — **stale for NOTRACE**: the I-39
  debug gate refuses a NOTRACE target (sub-kernel-devproc).
- **The rest is forward-compat scaffolding, generically covered.** `PROC_FLAG_NODUMP`
  / `MLOCKED` still await their enforcing subsystems (no core-dump or swap subsystem
  exists in the tree), and the escalate-then-crash-dump one-way refusal
  (`SET_DUMPABLE(1)` after `(0)` denied) is carried as the generic monotonic
  one-way `proc_flags` property in sub-kernel-proc — a flag that cannot be weakened.
  The thin handlers are `sub-kernel-syscall-dispatch`'s. Zero code change.
