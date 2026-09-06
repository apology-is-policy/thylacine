# 65 — corvus skeleton (P5-corvus-bringup-a) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-corvus-clean-absorb`).
This chunk was the first corvus sub-chunk: the startup-hardening sequence and
joey's cap-delegating spawn. Both live in the daemon dossier:

- the **startup hardening** (lock memory, disable core dumps, disable tracing,
  prove the CSPRNG answers, wipe the probe) — the fail-closed, boot-fatal,
  fixed-order sequence corvus runs before it serves — and the joey cap-grant
  chain that hands it exactly `CAP_LOCK_PAGES | CAP_CSPRNG_READ`:

      vault/system/userspace/services/sub-corvus.md

The five hardening *syscalls* themselves (`mlockall` / `set_dumpable` /
`set_traceable` / `getrandom` / `explicit_bzero`) are the kernel's surface, not
corvus's — their as-built home is owed a dossier of its own (the
`58-corvus-syscalls` authoring queue), not this file.

**What this file got WRONG or MISSED by the time it was absorbed:**

- Its caveat that `set_dumpable(0)` / `set_traceable(0)` are "forward-compat: at
  v1.0 there are no core-dump or debug-Spoor subsystems to gate" is **stale**.
  The I-39 debug-fs landed (see the debug-fs dossier), and it refuses to attach
  to a `PROC_FLAG_NOTRACE` target — so `set_traceable(0)` is now a live,
  enforced gate, not a placeholder.
- Its whole shape is the skeleton that **exits 0 after hardening**; production
  corvus is the long-lived 9P server the dossier describes, and the "exit 0" was
  a test signal the very next sub-chunk replaced with the serve loop.
