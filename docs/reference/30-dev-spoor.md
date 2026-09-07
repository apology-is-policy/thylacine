# 30 — Dev + Spoor (the device vtable and the channel) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-dev-spoor-absorb`).
This document bundled three concerns the vault keeps as three dossiers, so it
redirects to **all three** — each owns exactly its part, and together they carry
everything this file documented (verified: no content orphaned):

- the **Dev vtable** (`kernel/dev.c`, `kernel/devnone.c` — the `bestiary`,
  `dev_register`/`dev_init`, the 25 vtable slots incl. the `.wstat_native` /
  `.perm_enforced` registration gate, `dev_simple_*`, `devnone`):

      vault/system/kernel/namespace/sub-kernel-dev.md

- the **Spoor lifecycle** (`kernel/spoor.c` — the `clunk`-vs-`unref`
  distinction and the shallow-`aux` failure-unwind, the ref-in-walk rule,
  `walkqid_alloc`, magic-at-offset-0 UAF defense, the flag
  provenance-vs-state classification):

      vault/system/kernel/namespace/sub-kernel-spoor.md

- the **`Spoor.path` / I-33** half (`kernel/path.c` — the copy-on-walk `Path`,
  `path_addelem`, the fail-soft NULL semantics, the three hooks, the `/`-seed
  at attach, `FD2PATH`):

      vault/system/kernel/namespace/sub-kernel-path.md

**What this file got WRONG or MISSED by the time it was absorbed** (the reason
the dossiers are written from the code):

- Its Dev vtable is stale at **16 slots**; the vtable is now **25** (the
  `.wstat_native` / `.perm_enforced` gates and the later device families added
  them). `sub-kernel-dev` carries the current set.
- It bundles the `Path` (#66 / I-33) surface into the Spoor chapter as though
  it were one concern; the vault separates it because a wrong/stale/absent
  `Path` changes only the cosmetic introspection readers, never a resolution or
  permission result — the write-only-to-Path property I-33 rests on, which the
  bundling obscured.
- Its historical naming asides (the `Chan` -> `Spoor`, `devtab` -> `bestiary`
  renamings) are dropped deliberately; the current names stand on their own.
- The exact vtable slot roster and `struct Spoor` / `struct Path` byte layouts
  live in the headers (`kernel/include/thylacine/{dev,spoor,path}.h`) — the
  source of truth — which the dossiers point at rather than duplicating.
