---
paths:
  - "kernel/**"
  - "arch/**"
  - "mm/**"
---

# Kernel code patterns

> Moved from `CLAUDE.md` on 2026-09-23. Loads when kernel/arch/mm files are read.

### Idempotency on retry

Any function that writes durable state MUST short-circuit on clean state. If the function's contract is "on success, durable state X is recorded," then calling it twice with the same inputs and no intervening mutations must produce byte-identical durable state.

**Pattern**: carry a dirty flag. Mutations set dirty. Commits check dirty; if clean AND a durable result already exists, return cached result. If dirty, do the work + clear dirty.

### Compile-time invariants

Every on-disk, on-wire, or ABI-exposed format gets:
- `_Static_assert` (C/C++) on struct size, alignment, and discriminant ranges.
- Explicit version constants.
- Compat / ro-compat / incompat feature-flag tiers (where applicable).

Catches format drift at build time, not at runtime.

For Thylacine specifically:
- ELF loader: `_Static_assert` on ARM64 e_machine, ABI version.
- 9P wire format: `_Static_assert` on message header sizes, fid widths, tag widths.
- Handle table layout: `_Static_assert` on `struct Handle` size + alignment.
- Page table entry bit layout: `_Static_assert` on PTE bit positions (W^X invariant).
- DTB parse: `_Static_assert` on FDT magic, version expectations.

### Crash-injection + fault-injection testing

For torn-write-sensitive paths (Stratum mount transition, persistent state machines, multi-phase commits), wire fault-injection hooks at every durable write. Test that recovery from each injection point produces a valid state. Same pattern applies to interrupt injection in schedulers, fault injection in fault-tolerant networking, and partial-failure injection in distributed systems.

For Thylacine: kernel panic during ramfs → Stratum transition; driver process kill mid-IO; 9P session drop mid-walk.

---
