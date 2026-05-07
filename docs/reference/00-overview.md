# 00 — Overview

This is the as-built bird's-eye view of Thylacine OS. It is updated per chunk during implementation. Currently scaffolded; full content lands as subsystems implement.

---

## Layer cake (bottom-up)

```
(scaffolded; redrawn per chunk during Phases 1-8)

Target end state at v1.0:

  ┌─────────────────────────────────────────────────────────┐
  │  Halcyon (Phase 8)                                      │
  │  Scroll-buffer shell, image, video                      │
  ├─────────────────────────────────────────────────────────┤
  │  Userspace services (Phase 5-7)                         │
  │  janus, drivers, video player, network stack,           │
  │  POSIX-compat 9P servers, container runner              │
  ├─────────────────────────────────────────────────────────┤
  │  Compat layer (Phase 5-6)                               │
  │  musl libc port, Linux ARM64 syscall shim               │
  ├─────────────────────────────────────────────────────────┤
  │  Kernel (Phase 1-4)                                     │
  │  Territory, EEVDF, VM, 9P client (pipelined),           │
  │  Handle table, BURROW manager, IRQ forwarding, Notes,      │
  │  Dev, Spoor, Pipes, rendezvous, PTY infrastructure       │
  ├─────────────────────────────────────────────────────────┤
  │  Stratum (Phase 4 dependency; external)                 │
  │  PQ-encrypted, formally verified, COW filesystem        │
  └─────────────────────────────────────────────────────────┘
```

---

## Cross-cutting concerns

### Subordination invariant (handles ↔ 9P)

(One paragraph describing the invariant + which specs pin it + which code paths uphold it. Filled in when handles land in Phase 2.)

Reference: `ARCHITECTURE.md §18.1`.

### W^X

(One paragraph. Filled in when memory subsystem lands in Phase 1-2.)

Reference: `ARCHITECTURE.md §6.6`, invariant I-12.

### Lock order (global)

(The global lock-acquisition order with any reversal-warnings. Filled in when SMP scheduler lands in Phase 2.)

Reference: `ARCHITECTURE.md §20`.

### KASLR / ASLR

(Filled in when boot subsystem lands in Phase 1.)

Reference: `ARCHITECTURE.md §5.3`, `§24`.

### 9P session lifecycle

(Filled in when 9P client lands in Phase 4.)

Reference: `ARCHITECTURE.md §10.2`, `§21`.

---

## Versioning

| Version | Bump reason |
|---|---|
| (none yet) | Phase 0 complete; v0.1 lands at Phase 1 exit. |

---

## Phase status (as-built)

| Phase | Status | Highlights | See |
|---|---|---|---|
| 0 | ✅ complete | VISION + COMPARISON + NOVEL + ARCHITECTURE + ROADMAP + TOOLING + CLAUDE.md scripture | (the scripture itself) |
| 1 | 🚧 in progress | P1-A/B/C landed: boot stub, UART, banner, Linux ARM64 image header, DTB parser, DTB-driven hardware discovery, MMU on with W^X, extinction (kernel ELE) infra. P1-C-extras (KASLR + guard page + EL2 drop) and P1-D (physical allocator) next. | phase1-status.md, reference/01-boot.md, reference/02-dtb.md, reference/03-mmu.md, reference/04-extinction.md |
| 2 | — | Process model + EEVDF scheduler + handles + BURROW | (planned) |
| 3 | — | Userspace VirtIO drivers (no in-kernel virtio-blk) | (planned) |
| 4 | — | 9P client + Stratum mount | (planned) |
| 5 | — | Syscalls + musl + uutils + **Utopia** ships | (planned) |
| 6 | — | Linux compat + network + container runner | (planned) |
| 7 | — | Hardening + audit + 8-CPU stress + **v1.0-rc.1 tag** | (planned) |
| 8 | — | Halcyon + v1.0 final | (planned) |

---

## Test posture

- Test suite count: 0 (test harness lands in P1-I).
- Sanitizer matrix: not yet enabled (per-phase enablement; ASan + UBSan from P1-I, TSan from Phase 2 SMP).
- Spec count: 0 written; 9 planned per `ARCHITECTURE.md §25.2`.
- Manual verification at P1-A: boot-banner integration check via `tools/run-vm.sh`.

(Refreshed per chunk during implementation.)

---

## Build + CI

- Local invocation: `tools/build.sh kernel` + `tools/run-vm.sh`. See `CLAUDE.md` "Build + test commands" for the canonical reference.
- CI workflow: not yet configured (P1-I deliverable — auto-build + boot-banner check + sanitizer matrix).
- Toolchain: clang 22 + ld.lld 22 + cmake 4 (Apple Silicon Mac via Homebrew). Cross-compile target `aarch64-none-elf`.

---

## Revision history

| Date | Change | Reason |
|---|---|---|
| 2026-05-04 | Scaffolded (Phase 0 complete). | Bird's-eye overview template. Per-chunk updates begin at Phase 1 entry. |
