# 52 — SYS_PIPE [ABSORBED INTO THE VAULT]

Absorbed at the memory/ipc-wake sweep (`chg-2026-08-01-mm-ipc-sweep`).
Its content now lives in:

    vault/system/kernel/ipc-wake/sub-kernel-pipe.md

(the Contract section carries the syscall: two fds via x0/x1,
R|W|TRANSFER both ends with the wrong-end gate in the Dev, the
rollback discipline.)

**What this file got WRONG by the time it was absorbed.** Deep-frozen
at P5-fd-pipe, a doc about a world where the syscall had just been
born:

- "Without SYS_READ / SYS_WRITE / SYS_CLOSE syscalls, userspace can
  `pipe()` but can't actually use the fds" — every named syscall
  landed eras ago; the Status table's five "Deferred" rows all
  landed.
- "`uaccess_store_u32` doesn't yet exist; only uaccess_load_u8 is
  wired" — store_u32 landed with #112, and CF-3 added bulk
  copy_in/copy_out.
- `PROC_HANDLE_MAX = 64` — the constant has moved twice since this doc
  froze: 256 at the go-arc growth, **1024** at the #198 fid-ceiling
  chain. Stated as history rather than as a fresh number, because the
  first version of this correction said "256 since the go-arc growth"
  and rotted the same way inside a month.

The handler listing itself is still shape-accurate (rights,
rollback), which is why the dossier absorbs rather than corrects it.
