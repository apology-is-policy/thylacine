# 125 — Weft: the capability network dataplane (I-37) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-weft-doc-absorb`).
Weft replaces the confined-Proc-to-daemon byte copy with a page both Procs map:
the daemon does the capability work once, at grant, and the bytes then move
through shared memory with nothing mediating each operation. The isolation is the
grant; the speed is the *absence* of per-operation mediation. This is the tree's
first shared page. Audit-bearing; enforces **I-37** (dataplane integrity) +
**I-30** (the snapshot discipline) + **I-9** (the readiness no-lost-wake) +
**I-32** (the cross-Proc pin budgets). Its content lives, code-verified and
current, in:

- the **whole kernel Weft mechanism** — the three-call Share/Map/Unshare contract
  (the identifier never reaches the client — the remote-memory-key shape), the
  four minted-not-asserted shareable kinds (anon ring / framebuffer / gpu_bo /
  the HOSTMEM Venus ring — and the V-2/V-3b-1c-2b-F1 half-widen that both reading
  sites must close in lockstep), the-pin-is-the-lifetime, the private-view I-30
  geometry, the drain snapshot (copy-validate-act, extent summed wide), the I-9
  readiness poke (the store-buffer register-then-observe across a Proc boundary),
  the F_NOTIF three-holder zero-copy-send completion, the orphan reaper, and the
  kind-gate-is-the-single-chokepoint for all three data-drive consumers:

      vault/system/kernel/async/sub-kernel-weft.md   (guarded-by inv-i37/i30/i9/i32)

- the **synchronous Tweftio data-drive** — the `SYS_WRITE`/`SYS_READ` kick (no new
  syscall) gated on `WEFT_HYBRID_THRESHOLD`, `dev9p_weft_try_write` /
  `dev9p_weft_try_read` (the RX writes recv'd bytes straight into the guest's ring
  — no `uaccess_store` copy-out, the true zero-copy), all reusing `weft.c`'s
  `weft_binding_validate_rw` (the same private-view gate as the drain, but sourced
  from trusted register-passed args so inherently free of the descriptor-ring
  TOCTOU), plus the `Tweftio` wire op:

      vault/system/kernel/ninep/sub-kernel-ninep-dev9p.md
      vault/system/kernel/ninep/sub-kernel-ninep-wire.md
      vault/system/boundary/registries/abi-ninep-wire.md   (the Tweftio op, a PIN)

- the **netd half** — `h_weft` (the Tweft ring register) + `h_weftio` (read/write
  in place) + the RX empty-but-open `PendingWeftRead` defer (the I-9 twin of the
  byte-copy `h_read` defer):

      vault/system/userspace/services/sub-netd-server.md
      vault/system/userspace/services/sub-netd-nic.md

- the **Loom data drive** — `LOOM_OP_READ`/`WRITE` → `Tweftio`, the batched/async
  submission the native `WeftFlow` push/pop/wait API rides:

      vault/system/kernel/async/sub-kernel-loom.md

- the **weave share (G-2 — the Tapestry surface-share generalization)** —
  `SYS_DMA_CREATE_WEAVE`, `burrow_share_into`, `SYS_WEFT_UNSHARE`, and the
  weave clunk-unmap (the kernel substrate is the burrow-share; the surface-share
  consumer is Tapestry):

      vault/system/kernel/memory/sub-kernel-burrow.md
      vault/system/userspace/services/sub-tapestryd.md
      vault/system/userspace/runtime/sub-libtapestry.md

- the **shared-in budget (the I-32 fifth axis)** — the client's cross-Proc pin
  charged to a dedicated per-AddrSpace budget:

      vault/system/kernel/memory/sub-kernel-addrspace.md
      vault/system/kernel/execution/sub-kernel-proc.md

- the **formal models** — `specs/weft.tla` (dataplane) + `specs/weft_readiness.tla`
  (the I-9 no-lost-wake), clean + liveness + buggy cfgs.

**What this file got WRONG or MISSED by the time it was absorbed:**

- **The opening prose self-describes as an earlier sub-chunk.** The header and the
  file's first line call themselves "the substrate: the descriptor-ring ABI and
  the validating consumer" and list the delivery calls as future Weft-6 work — but
  the share registry, the claim path, the framebuffer kind and the orphan reaper
  are all already in `weft.c`. The third consecutive area (with the console and
  the entry region) where the summarizing prose lags the code, whose per-function
  comments carry their audit references and are current. `sub-kernel-weft`'s
  Caveats already record this drift.
- **The "10x slower" performance claim was wrong and is corrected** — against the
  copy path at matched size the aggregate is a dead heat (the data movement itself
  ~2x faster over ~half the operations); `sub-kernel-weft`'s Performance section
  carries the corrected number.
- **The content is distributed** — the kernel mechanism to `sub-kernel-weft`, the
  data-drive to `sub-kernel-ninep-dev9p` / `-wire` / netd, the weave share to
  `sub-kernel-burrow` / Tapestry, the shared-in budget to `sub-kernel-addrspace` /
  `sub-kernel-proc`, the models to the two `weft*.tla` specs.
