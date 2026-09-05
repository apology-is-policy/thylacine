# 51 — Kernel pipe [ABSORBED INTO THE VAULT]

Absorbed at the memory/ipc-wake sweep (`chg-2026-08-01-mm-ipc-sweep`).
Its content now lives, code-verified and current, in:

    vault/system/kernel/ipc-wake/sub-kernel-pipe.md

(the ring + endpoint pair, the blocking loops and their four wakes,
the atomic ref, the #96 fstat identity, the poll integration, the
rollback ladder.)

**What this file got WRONG by the time it was absorbed.** The best
single specimen of the corpus asserting two different values for one
pinned struct: this file shows the PRE-blocking field list (no lock,
no rendezes, no EOF flags, no poll list) and pins the size at
**72 + 4096** in prose — while `72-poll.md` next door documented the
true **88 + 4096** layout offset-by-offset. Three eras of one
`_Static_assert`.

- The allocation caveat says "order-2 = 16 KiB, 12 KiB waste" — wrong
  when written (4128 bytes is 2 pages → order 1) and wrong now
  (4184 → order 1 = 8 KiB, ~4 KiB slack).
- The Performance section says "No locking at v1.0 (single-CPU)" two
  screens above the Status row recording the per-ring lock.
- No `.poll` (the vtable list says "other slots: stubs"), no #811
  INTR arms, no `notes_post_pipe`, no atomic-ref discipline in the
  struct comment.
- "Userspace pipe(2) syscall — deferred to P5-fd-syscalls" and every
  sibling deferral row long landed.

One CODE header carries the sharpest version: `kernel/include/
thylacine/pipe.h`'s own semantics block still describes the
non-blocking P5-pipe behavior ("neither end blocks; read returns 0
if empty") as CURRENT, with blocking as future — inverted by the
very next chunk and never updated. Recorded as a dossier caveat for
main-track fixing.
