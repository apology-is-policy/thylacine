# Phase 6 — status and pickup guide

Authoritative pickup guide for **Phase 6: Pouch — POSIX libc + cross-compilation** (execution Phase 6 per `ROADMAP.md §2.1`; ROADMAP section `§7A`). Binding design: **`docs/POUCH-DESIGN.md`**.

## TL;DR

**Phase 6 is OPEN — design landed, implementation not yet started.** The design session (3 rounds, 2026-05-22) converged; `POUCH-DESIGN.md` is binding scripture.

Phase 6 builds **pouch** — the Thylacine-native POSIX libc (a musl derivative: musl's portable upper half + a Thylacine-native lower half, split at musl's syscall seam) — plus the `aarch64-thylacine` cross-compilation toolchain + sysroot, plus the POSIX runtime layer that translates POSIX abstractions into Thylacine primitives (`AF_UNIX`→`/srv`, `poll(2)`→`t_poll`, `sigaction`→notes, pthreads→Thylacine threads + the `torpor` wait-on-address primitive). The proving binary is real **stratumd**; the durable deliverable is the cross-compilation path itself.

Phase 6 **preempts the Phase 5 close**: Phase 5 (9P client + Stratum integration) is suspended, substantially complete — its tail (real stratumd, P5-hostowner-c, P5-login) is pouch-dependent. When Phase 6's stratumd criteria are met, the bulk of Phase 5's own exit criteria are satisfied too.

Why a whole phase: Thylacine's practicality as a real OS depends on POSIX/Linux/BSD software reuse; that capability is phase-sized and load-bearing. Stratum is a Thylacine-native-primary project, so a first-class Thylacine build+run target for it is core, not accommodation.

## Phase entry pickup (read order)

1. `docs/POUCH-DESIGN.md` — the binding design. Read in full; §18 is the decision log.
2. `docs/ROADMAP.md §7A` — the ROADMAP-level phase summary.
3. `docs/ROADMAP.md §3.6` — "the compat layer is built on top, not baked in" — the principle pouch makes structurally true (invariant P-1).
4. `docs/ARCHITECTURE.md §16` + `§23` — the POSIX-compat + POSIX-surfaces architecture pouch realizes.
5. `memory/project_pouch_phase.md` — the phase decision + rationale.
6. The 3 research reports informing the design (summarized in POUCH-DESIGN.md §1 + the risk register).

## Landed chunks

| Commit SHA | What | Tests |
|---|---|---|
| *(none yet — implementation begins at sub-chunk 1)* | | |

## Remaining work — the 15 sub-chunks

Per `POUCH-DESIGN.md §14`. Each lands independently with the two-commit pattern; audit-bearing ones get a focused round.

| # | Chunk | Audit-bearing |
|---|---|---|
| 1 | `pouch-toolchain` — triple, sysroot layout, clang toolchain + `pouch-clang` wrapper | no |
| 2 | `pouch-musl-vendor` — vendor pinned musl; the boundary-line patch series scaffold | no |
| 3 | `pouch-kernel-auxv` — `exec_setup` auxv population; `set_tid_address` | yes |
| 4 | `pouch-syscall-seam` — retarget `bits/syscall.h`; 1:1 wrappers; rc→errno mapping | yes |
| 5 | `pouch-hello-smoke` — static + printf hello build + run | no |
| 6 | `pouch-mem` — allocator backend (anonymous-memory call) | yes |
| 7 | `pouch-wait-addr` — the `torpor` kernel primitive; `futex.tla` (spec-first) | yes |
| 8 | `pouch-threads` — pthread create/join/detach + mutex/cond/rwlock/once + TLS errno | yes |
| 9 | `pouch-poll` — `poll`/`select`/`ppoll` → `t_poll` | no |
| 10 | `pouch-devnodes` — minimal synthetic-FS namespace; trivial `/dev` nodes | no |
| 11 | `pouch-sockets` — `AF_UNIX` `SOCK_STREAM` → `/srv`; `SO_PEERCRED` → `t_srv_peer` | yes |
| 12 | `pouch-signals` — the supported signal subset → notes | yes |
| 13 | `pouch-libsodium` — cross-compile libsodium; self-test | no |
| 14 | `pouch-stratumd-build` — build stratumd against the sysroot; Thylacine `peer_creds` arm | no (Stratum-side) |
| 15 | `pouch-stratumd-boot` — joey spawns real stratumd; `/sysroot` mount; ramfs pivot; retire the stub | yes |

Critical path + highest risk: chunks 7–8 (`torpor` + threads); each may split. Chunk 7 is spec-first.

## Exit criteria status

Full list in `POUCH-DESIGN.md §13`. None met yet (implementation not started). Checklist:

- [ ] `aarch64-thylacine` cross-toolchain builds; `tools/build.sh sysroot` produces a populated sysroot.
- [ ] Static "hello" C program runs in Thylacine — prints, exits 0, no leak.
- [ ] `printf`-shaped hello works (buffered stdio path).
- [ ] Multithreaded test (N threads, shared mutex-protected counter, join) passes under default + TSan.
- [ ] `AF_UNIX` `SOCK_STREAM` echo client/server (pure POSIX source) round-trips over `/srv`.
- [ ] libsodium cross-compiles + self-test passes.
- [ ] stratumd compiles + links against the sysroot.
- [ ] stratumd boots, binds its `/srv` FS socket, serves 9P; the Phase-5 stub is retired.
- [ ] joey mounts `/sysroot` from real stratumd; ramfs→Stratum pivot completes.
- [ ] No P0/P1 audit findings on pouch's lower half or the kernel additions.
- [ ] The patch series against vendored musl is documented + reproducible.

## Build + verify commands

Standard (per `CLAUDE.md`): `tools/build.sh kernel [--sanitize=ubsan]` then `tools/test.sh` (`BOOT_TIMEOUT=420` for UBSan). New for this phase: `tools/build.sh sysroot` (becomes real at sub-chunk 1). TSan matrix matters from chunk 8 (`pouch-threads`) onward.

## Trip hazards

- **Chunks 7–8 are the hardest.** The `torpor` wait-on-address primitive + the pthread mutex/condvar layer. Spec-first (`futex.tla`); TSan from the start. Either may split.
- **musl's lower half may resist a clean seam** (POUCH-DESIGN.md risk R2). Chunk 2 is exploratory; if the seam is dirtier than the upper/lower model assumes, the patch series grows.
- **Invariant P-1**: no foreign syscall number ever enters the kernel. Every kernel addition this phase is a *native Thylacine* syscall designed on Thylacine's terms — never "the Linux futex ported."
- **Stratum-side coordination** (chunks 14–15): the Thylacine `peer_creds` arm + possibly the block-layer arm are Stratum-repo changes.

## Known deltas from POUCH-DESIGN.md

None yet — implementation has not started. Deltas surfaced during implementation are recorded here per the deviation-tracking discipline.

## References

- `docs/POUCH-DESIGN.md` — binding design (authoritative).
- `docs/ROADMAP.md §7A` + `§2.1` + `§3.6`.
- `docs/ARCHITECTURE.md §16`, `§23`, `§25` (audit triggers), `§28` (invariants — pouch P-1..P-4 cross-ref).
- `docs/VISION.md §12` — Relationship to Linux and POSIX.
- `memory/project_pouch_phase.md`.
- Specs: `futex.tla` (#7), `notes.tla` (#8), `poll.tla` (#6), `pty.tla` (#9).
