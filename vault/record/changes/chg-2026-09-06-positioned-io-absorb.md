---
id: chg-2026-09-06-positioned-io-absorb
type: chg
title: "absorb docs/reference/130-positioned-io (SYS_PREAD/PWRITE #37 + wstat #47): fold the positioned-I/O syscall mechanism into sub-kernel-syscall-dispatch, multi-redirect stub"
date: 2026-09-06
arc: arc-vault
commits: ["1ebe5569"]
touched: [sub-kernel-syscall-dispatch]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-06
---
The positioned byte-I/O pair (SYS_PREAD=85 / SYS_PWRITE=86) + the SYS_WSTAT #47
kind-gate. Verified atom-by-atom before stubbing.

WHERE EACH ATOM LIVES (verified, not assumed):
- The ABI numbers + the SYS_RW_MAX=4096 clamp -> sub-kernel-syscall-abi.
- The Dev-half `seekable = true` (devramfs + dev9p honoring the offset) + the
  prw_wire_offset_and_cursor test -> sub-kernel-ninep-dev9p.
- The wstat_native/perm_enforced dev_register-extinct pin (audit F1) ->
  sub-kernel-dev / sub-kernel-content / sub-kernel-ninep-dev9p (all three).
- SYS_WSTAT #47 (kind-gate-not-rights-gate, T_WSTAT_SIZE content/metadata split)
  -> ALREADY folded into sub-kernel-syscall-dispatch by
  chg-2026-09-06-fs-permission-absorb.

THE FOLD (the load-bearing atom that lived only in the doc):
The positioned-I/O SYSCALL-LAYER mechanism was uncovered -- dev9p carried the
seekable flag, syscall-abi the numbers, but the handler-level machinery lived
only in the reference doc:
1. The cursor-untouched contract: the shared spoor_read_common/spoor_write_common
   inner + a `positioned` flag (cleared = read+advance c->offset; set = pass the
   caller's offset through, touch the cursor on no path). This is what
   io.ReaderAt's documented parallel-use guarantee rides on -- no Seek+Read
   emulation can provide it.
2. The three ordered gates: off<0 (before the lookup); non-seekable -> POSIX
   ESPIPE shape, checked BEFORE the len==0 short-circuit (so a zero-length probe
   on a stream Dev reports the refusal rather than succeeding as a no-op);
   off+len > INT64_MAX overflow guard.
3. The repeat-safe asymmetry: a copy-out fault mid-pread loses nothing (the
   cursor never moved), unlike SYS_READ's consumed-bytes-lost window.
4. The consumer-side Go divergence (F2): the Go syscall.Pread/Pwrite wrapper
   returns (0, nil) on len==0 rather than trapping (indexing an empty slice
   panics), so a zero-length positioned op on a non-seekable fd is seen as
   success by that one caller where the kernel reports the ESPIPE-shaped -1.

Folded into sub-kernel-syscall-dispatch (audit: hard; owns kernel/syscall.c where
the shared inner + the gates live) as a new FS-handler Mechanism subsection, plus
a Prosecution bullet on the gate order.

NOT REFUTED: the doc's ABI, gates, and both audit findings match the code. The
gap was ownership of the syscall half. Zero code change. Multi-redirect stub.
