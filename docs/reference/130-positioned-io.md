# 130 — Positioned byte I/O: SYS_PREAD / SYS_PWRITE (#37) + the SYS_WSTAT kind-gate (#47) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-positioned-io-absorb`).
The positioned-I/O syscall pair (`SYS_PREAD` = 85 / `SYS_PWRITE` = 86 — byte I/O
at an absolute offset that never touches the fd cursor) and the wstat
rights-posture correction, landed as one chunk. Its content lives, code-verified
and current, in:

- the **syscall-layer mechanism** — the cursor-untouched contract (the shared
  `spoor_read_common`/`spoor_write_common` inner + the `positioned` flag), the
  three ordered gates (`off < 0`; non-seekable -> POSIX ESPIPE shape, checked
  *before* the `len == 0` short-circuit; `off+len` overflow), the repeat-safe
  mid-`pread` fault asymmetry, and the Go-wrapper `len == 0` divergence —
  **folded here at this absorption** — plus the SYS_WSTAT #47 kind-gate-not-rights-
  gate split, folded earlier:

      vault/system/kernel/entry/sub-kernel-syscall-dispatch.md   (audit: hard)

- the **ABI** — the syscall numbers + the `SYS_RW_MAX = 4096` per-call clamp that
  applies to read/write/pread/pwrite alike:

      vault/system/kernel/entry/sub-kernel-syscall-abi.md

- the **Dev half** — `seekable = true` (devramfs + dev9p) honoring the byte
  offset, and the `dev9p.prw_wire_offset_and_cursor` wire-capture test:

      vault/system/kernel/ninep/sub-kernel-ninep-dev9p.md

- the **wstat soundness pin** — `dev_register` extincts a `wstat_native` Dev that
  is not `perm_enforced` (audit F1), since `perm_wstat_check` is the only
  write-authority gate on that path:

      vault/system/kernel/namespace/sub-kernel-dev.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **The positioned-I/O syscall mechanism was uncovered — folded at absorption.**
  The Dev-half (`seekable` flag) was in `sub-kernel-ninep-dev9p` and the ABI
  numbers in `sub-kernel-syscall-abi`, but the *handler-level* mechanism — the
  shared `positioned`-flag inner, the ORDER of the three offset gates (the
  non-seekable refusal deliberately preceding the `len == 0` short-circuit so a
  zero-length probe still reports ESPIPE), and the cursor-untouched contract that
  `io.ReaderAt` parallel-use rides on — lived only here. Now folded into the
  dispatch dossier's FS-handler Mechanism, with a Prosecution bullet on the gate
  order.
- **The doc is current** — the ABI, the three gates, and the two audit findings
  (F1 `wstat_native` => `perm_enforced`; F2 Go `len == 0` skips the ESPIPE trap)
  all match the code. Nothing was refuted; the gap was ownership of the syscall
  half. Zero code change.
