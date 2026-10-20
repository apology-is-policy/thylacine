---
id: dec-2026-10-07-maps-code-redaction
type: dec
title: "/proc/<pid>/maps prints a code row's addresses only for a reader with debug authority"
date: 2026-10-07
status: standing
decided-by: user-vote
affects: [sub-kernel-devproc, sub-kernel-mmu]
created: 2026-10-07
---
## Fork

B-2b places every code alias -- writer, exec, sealed -- at its own random
address ([[dec-2026-10-07-jit-sealed-thunk]]), so the writer's address is a
secret for the first time. `/proc/<pid>/maps` lists every VMA with its
addresses and is ambient: mode 0444, all pids, for any unsealed Proc. Its own
comment carried the obligation: "if user ASLR ever lands, this posture must be
revisited in the same chunk". Left alone, any Proc reads another's writer
address (B-2b audit r1 F2, widened in triage).

## Research

- Plan 9: `/proc/n/segment` is 0444 plus the proc's mode bits, readable by
  all; Plan 9 has no address randomisation, so it has nothing to hide.
- Linux: `proc_maps_open` takes `proc_mem_open(inode, PTRACE_MODE_READ)`, so
  `maps` needs ptrace-read rights (same credentials and a covering capability
  set, or `CAP_SYS_PTRACE`). `/proc/<pid>/stat` stays world-readable but prints
  `start_code`, `end_code`, `start_stack` and the stack and instruction
  pointers as 0 or 1 when the reader lacks the same right.
- Tree facts: the I-39 predicate `devproc_debug_authorized_locked` is
  Thylacine's `PTRACE_MODE_READ` -- reflexive, owner-covers over the image
  join, or `CAP_HOSTOWNER` / `CAP_DEBUG`, and refused for a NOTRACE image.
  The diorama (`usr/diorama/src/server.rs`) renders a Linux guest's
  `/proc/self/maps` by reading the native file as ITSELF, and a
  Linux-phenotype Proc cannot create a code region (no syscall row; `CAP_JIT`
  gates the native one). Nothing else in `maps` is random: every other address
  is an `exec.h` constant, an ELF link address or a first-fit placement.

## Options

1. **Redact the code rows.** A code row keeps its permissions and type; its
   addresses print as zero unless the reader passes the I-39 predicate. The
   rest of `maps` stays ambient.
2. **Gate all of `maps`** on the I-39 predicate, Linux's posture. A Linux
   guest's `/proc/self/maps` would go empty through the diorama unless the
   diorama held debug authority over every guest.
3. **Gate `maps` only for a target holding a code region.** The file's
   posture would depend on what the target has mapped.

## The call

Option 1 (operator, 2026-10-07, AskUserQuestion). Rides with B-2's land, in
B-2b.

## Rationale

Only the code rows carry a secret, so only they need a gate; withholding the
rest of the table would cost diagnostics and the diorama's guests for nothing
it protects. The redacted rows are printed after every other row, grouped by
permission, with their offset zeroed as well: a row's place in an
address-sorted listing would tell a reader which mappings its alias sits
between, and in what order the writer and exec aliases lie. Each redacted row
is then a function of its permissions alone, so a reader without debug
authority learns how many code aliases of each kind the target holds and
nothing about where they are or how large they are.
