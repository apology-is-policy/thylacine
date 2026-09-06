# 107 — Loom: the io_uring-inverted 9P ring transport (I-29 / I-30) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-loom-doc-absorb`).
Loom is the shared-memory alternative to a syscall-per-file-operation: userspace
writes operation descriptors into a ring the kernel reads, the kernel's 9P engine
runs them, and replies come back as completion entries — a batch of work for one
trap, or (with the poll thread) none. The inversion is that the opcodes are the
9P client's own surface, so one async layer covers files / network / proc / srv /
dev. Audit-bearing; enforces **I-29** (completion integrity) + **I-30**
(submit-time pin) + **I-32** (the ring's page + thread budgets). Its content
lives, code-verified and current, in:

- the **whole Loom mechanism** — the private-counter-is-authority ring geometry
  (I-30 TOCTOU: never index from a shared word), copy-first-then-decide, pin-at-submit
  / never-re-resolve-at-completion (the io_uring credential-vs-work class avoided),
  back-pressure-at-submit (the completion reservation), the callback-may-not-sleep
  discipline, multishot + the terminal-CQE-clears-the-flag, LINK/DRAIN ordering,
  the poll thread + its park condition, the borrow guard, the join, and the **I-32
  dual charge-ledger** (the two owner pointers bound at opposite ends of setup; the
  region-not-ring "who paid"; the thread-ledger backstop and the wrong one-line
  fix that would have leaked):

      vault/system/kernel/async/sub-kernel-loom.md   (guarded-by inv-i29/i30/i32)

- the **device-gone terminal** (the I-29 device-gone extension, Menagerie step 4)
  — `client_mark_dead_locked(c, bool devgone)`, the recv-0 → `-T_E_NODEV` vs
  recv-`-1` → `-T_E_IO` reason discrimination, `p9_client_mark_devgone`, and the
  exactly-once inflight-clear-before-complete — folded at absorption into the 9P
  client dossier (its code lives in `9p_client.c`, not `loom.c`); the transport
  recv-0-vs-`-1` contract is the session-transport dossier's:

      vault/system/kernel/ninep/sub-kernel-ninep-client.md   (the reason + devgone entry)
      vault/system/kernel/ninep/sub-kernel-ninep-transport.md (the recv contract + srvconn EOF)

- the **ring ABI** — the header / SQE / CQE / index-array layout and the opcode
  values the userspace mirror pins:

      vault/system/boundary/registries/abi-loom-ring.md   (a PIN)

- the **formal models** — `specs/loom.tla` + `loom_multishot.tla` + `loom_order.tla`
  + `loom_devgone.tla` (`NoDoubleTerminal` / `DeathResultFaithful` /
  `SessionDeathCompletes`), each with clean + buggy cfgs.

The userspace `libthyla_rs::loom` API and the Tapestry seam are a consumer of the
above, owned by the graphics/libtapestry surface.

**What this file got WRONG or MISSED by the time it was absorbed:**

- **The device-gone terminal was in no dossier body — folded at absorption.** The
  reason discrimination (a clean EOF from a torn-down server/driver endpoint
  completes in-flight async ops `-ENODEV`, distinct from a transport `-EIO`;
  before step 4 both collapsed to `-1`) is a load-bearing I-29 leg with its own
  spec, but its code lives in `9p_client.c` (owned by `sub-kernel-ninep-client`),
  where the dossier had documented `client_mark_dead_locked` as the sole dead-setter
  yet omitted its `bool devgone` reason parameter. Now folded there.
- **The Status / header self-description understates the file.** This doc (and the
  source header) open by calling `loom.c` "the ring substrate… no op flows yet —
  the opcodes are reserved ABI"; fifteen of the twenty opcodes dispatch there, and
  the file has since grown the poll thread, multishot, ordering, registered
  buffers and the zero-copy routing. `sub-kernel-loom`'s Caveats already flag this
  drift (the same shape as the console's and entry area's header blocks).
- **The content is distributed** — the mechanism to `sub-kernel-loom`, the
  device-gone terminal to `sub-kernel-ninep-client` / `-transport`, the ring ABI
  values to `abi-loom-ring`, the models to the four `loom*.tla` specs.
