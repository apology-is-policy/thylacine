# 133 — The Go port (GOOS=thylacine): capability map [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-07-go-port-doc-absorb`).
A capability map for running the **real Go toolchain natively on-device** (`go`
driver + compiler + linker + stdlib in `/goroot`, `go build`/`go mod` in-guest,
modules pulled off `/net`). The **fork itself is external** —
`~/projects/go-thylacine` (go1.25.3 base, ~54 `*_thylacine*` files across
runtime/syscall/os/net/crypto/cmd-go) — **not a Thylacine-tree surface**, exactly
as the gopls port (137) is. What it documents that *is* in-tree lives, code-
verified and current, in:

- **the `T_WSTAT_SIZE` kernel lift** (§3a — the fourth `SYS_WSTAT` axis `go mod`'s
  in-place `.ziphash` truncation needed) — the content-vs-metadata split (SIZE
  demands `RIGHT_WRITE` and skips `perm_wstat_check`; the identity axes keep it),
  the `INT64_MAX` overflow bound, and the **#81-class `O_PATH`/`CWALKONLY` truncate
  reject** (an `O_PATH` handle's `RIGHT_WRITE` is hollow, so a truncate through it
  would bypass the write-permission check):

      vault/system/kernel/entry/sub-kernel-syscall-dispatch.md   (the handler-side reject + bound)
      vault/system/kernel/security/sub-kernel-perm.md            (SIZE-has-no-policy-arm)

- **the Loom SETATTR async twin** (§3a) — the audit finding that the async
  `LOOM_OP_SETATTR` path had the same `O_PATH` truncate bypass *plus* ran no
  identity check at all, closed by splitting on authority-kind: SIZE stays
  (RIGHT_WRITE, non-`O_PATH`, the s64 bound), MODE/UID/GID reject fail-closed →
  **v1.0 Loom SETATTR is truncate-only** (the async submit cannot run the sync
  owner-only `perm_wstat_check` without a blocking stat it may not make):

      vault/system/kernel/async/sub-kernel-loom.md   (audit: hard — folded here)

- **the env chain + boot probe** — login seeding `/env/{HOME,USER,PATH}`, the
  kernel-env child-inherit, and joey's go4c boot probe with its hermetic env
  unlinked after so it does not leak into a session:

      vault/system/stratum/sub-stratum-boot.md         (joey / the go4c probe)
      vault/system/stratum/sub-stratum-session.md      (login's session-env seed)

- **the native env read + nora integration** — `libthyla_rs::env::var` (the read
  side; there is deliberately no `set_var` — a program writes `/env/NAME` itself),
  and nora's gofmt-on-save (pipe-through-before-durable-write, checkpointed so one
  undo restores the pre-format text, never blocks or loses a save):

      vault/system/userspace/runtime/sub-libthyla-rs.md   (env::var + the set_var seam)
      vault/system/userspace/shell-tui/sub-nora-host.md   (gofmt-on-save)

**What this file got WRONG or MISSED by the time it was absorbed:**

- **One genuine gap, now folded — the Loom SETATTR truncate-only fail-close.** It
  appeared *nowhere* in the vault: the async-path authority split (SIZE by
  `RIGHT_WRITE`, MODE/UID/GID fail-closed because identity cannot be evaluated at
  submit) is a real audit-bearing security property, now in `sub-kernel-loom`'s
  submit-time-pin section (`chg-2026-09-07-go-port-doc-absorb`).
- **The fork's own findings are external, not tree surfaces.** The `capMask = ^0`
  os/exec inherit fix (a zero-value `ProcAttr` spawned Go children
  capability-naked → `CAP_CSPRNG_READ`-gated getrandom died EPERM on the first TLS
  module-fetch), the resolver-ORDER `net/conf.go` plan9-gate fix, and the
  `os.Executable` Args[0] model (advisory, not kernel truth — a `/proc/<pid>/text`
  surface is the v1.x upgrade for anything security-relevant) all live in
  `~/projects/go-thylacine`. The proof tiers (PROVEN/EXPECTED/STUBBED per surface),
  the CHASE build-perf record, and the netpoll-is-blocking / deadlines-don't-abort
  seams are the fork's + `docs/GO-PORT-PLAN.md`'s. Zero Thylacine-tree code change
  beyond the doc→dossier fold.
