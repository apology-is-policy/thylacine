# Phase 6 — status and pickup guide

Authoritative pickup guide for **Phase 6: Pouch — POSIX libc + cross-compilation** (execution Phase 6 per `ROADMAP.md §2.1`; ROADMAP section `§7A`). Binding design: **`docs/POUCH-DESIGN.md`**.

## TL;DR

**Phase 6 is OPEN — implementation underway.** The design session (3 rounds, 2026-05-22) converged; `POUCH-DESIGN.md` is binding scripture. **Sub-chunks 1-3 have landed** — sub-chunk 1 (`pouch-toolchain`) the `aarch64-thylacine` cross toolchain; sub-chunk 2 (`pouch-musl-vendor`) vendored musl 1.2.5 + the R2 probe (musl compiles end-to-end for `aarch64-thylacine`, so the boundary line is compile-clean); sub-chunk 3 (`pouch-kernel-auxv`, audit-bearing) made `exec_setup` build the System V process-startup frame (argc / argv / envp / auxv) at the top of the user stack and added `SYS_SET_TID_ADDRESS` — the two kernel-side primitives a static C runtime needs at startup. Next: sub-chunk 4 (`pouch-syscall-seam` — retarget `bits/syscall.h`, the rc→errno mapping; audit-bearing).

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
| *(pending)* | **pouch-kernel-auxv: hash fixup** | (placeholder fill) |
| *(pending)* | **P6-pouch-kernel-auxv (sub-chunk 3/15): `exec_setup` builds the System V process-startup frame (auxv); new `SYS_SET_TID_ADDRESS` syscall.** `exec_setup` (`kernel/exec.c`) now calls `exec_build_init_stack` to write a System V startup frame — argc=0, empty argv/envp, a six-entry auxv (`AT_PHDR`/`AT_PHENT`/`AT_PHNUM`/`AT_PAGESZ`/`AT_RANDOM`/`AT_NULL`) + 16 kernel-CSPRNG bytes for `AT_RANDOM` — into the top 144 bytes of the user stack; `*sp_out` now points at the frame's `argc` word. `elf_load` exposes `phoff`/`phnum`/`phentsize` on `struct elf_image` so `AT_PHDR` resolves to the program-header table's user VA (or 0 + a coherent zeroed triple when no segment covers it). New `SYS_SET_TID_ADDRESS = 36` returns the calling thread's tid (the Proc pid at v1.0 — the Linux thread-group-leader convention; `tidptr` accepted but deferred per POUCH-DESIGN.md §12.4). `<thylacine/elf.h>` gains the `AT_*` constants + `struct Elf64_auxv_t`; `<thylacine/exec.h>` gains `EXEC_INIT_STACK_SIZE`/`EXEC_INIT_AUXV_COUNT`/`EXEC_INIT_RANDOM_OFFSET`; libt gains `t_set_tid_address`. Freestanding binaries (joey, corvus, the probes) ignore the frame — their `_start` calls `main` directly — and boot unchanged. **Audit-bearing** (exec + ELF loader + syscall surface): a focused opus-prosecutor round returned 0 P0 / 0 P1 / 1 P2 / 4 P3, all five addressed (F1 — zero the whole `AT_PHDR`/`AT_PHENT`/`AT_PHNUM` triple when no segment covers the phdrs; F2 — refresh stale `_start` contract comments; F3 — document degraded-entropy behaviour; F4 — invariant `extinction` assert on the stack VMA lookup; F5 — strengthen the auxv-count `_Static_assert`). Closed-list: `memory/audit_p6_pouch_kernel_auxv_closed_list.md`. | 3 new kernel tests (535 → 538): `exec.setup_auxv` (full startup-frame inspection with a resolved `AT_PHDR`), `exec.setup_auxv_no_phdr_segment` (the `AT_PHDR`=0 fallback), `syscall.dispatch_set_tid_address`. **538/538 PASS × default + UBSan**, 0 UBSan runtime errors, full boot OK. |
| `45e287e` | **pouch-musl-vendor: hash fixup** | (placeholder fill) |
| `6f60b7e` | **P6-pouch-musl-vendor (sub-chunk 2/15): vendor musl 1.2.5; scaffold the boundary-line patch series; the R2 upper-half cross-compile probe.** musl 1.2.5 (sha256 `a9a118bb…c75e4`, MIT) vendored pristine + byte-identical at `third_party/musl/` (2697 files); new `third_party/README.md` records the vendoring policy + the per-package record. New `usr/lib/pouch/` is pouch's own tree: `patches/series` (quilt-style, empty), `patches/README.md` (the boundary-line discipline — invariant P-4), `README.md`. New `docs/reference/78-pouch.md` — the pouch subsystem reference: the boundary-line architecture + the full UPPER / LOWER / SEAM inventory of every musl `src/` subdir + `crt/` + `arch/aarch64/`. **R2 probe** (POUCH-DESIGN.md risk R2 — does musl take a clean seam): musl's `configure` accepts the `aarch64-thylacine` triple; out-of-tree `make lib/libc.a` compiles 1345 TUs, 0 errors / 0 warnings → a 2.4 MB `libc.a` of aarch64 ELF objects. The boundary line is compile-clean — the patch series' burden is semantic (replace what the lower half *does*), not a compile fight. **Vendor path resolved** (POUCH-DESIGN.md §4 left it open): `third_party/musl/`, not `usr/lib/pouch/musl/` — `third_party/` is the conventional pristine-upstream home; `usr/lib/pouch/` stays pouch's own code. `loc.sh` now excludes `third_party/`. Not audit-bearing — vendoring + docs + build scaffolding, no kernel code, no invariant surface. | No kernel-test-suite change (vendoring + scaffolding; no kernel code, test count stays 535/535). Verification: out-of-tree musl `make lib/libc.a` exit 0 — 1345 TUs, 0 errors / 0 warnings, 2.4 MB `libc.a`; `tools/build.sh sysroot` still exit 0 + toolchain self-check PASS. |
| `e03be8d` | **pouch-toolchain: hash fixup** | (placeholder fill) |
| `90b5333` | **P6-pouch-toolchain (sub-chunk 1/15): the `aarch64-thylacine` cross-compilation toolchain scaffolding.** New `cmake/Toolchain-aarch64-pouch.cmake` — clang + ld.lld (Homebrew LLVM); target `aarch64-thylacine` via `CMAKE_C_COMPILER_TARGET`; `CMAKE_SYSROOT` → `build/sysroot`; `-march=armv8-a+lse+pauth+bti` + hardening (stack protector, stack-clash, PAC+BTI) + `-nostdinc -isystem <sysroot>/include` via `CMAKE_C_FLAGS_INIT`; W^X + `-static` link flags via `CMAKE_EXE_LINKER_FLAGS_INIT`; compiler probes skipped (no libc to link against yet). New `tools/pouch-clang` — a drop-in `clang` wrapper pinned to `--target=aarch64-thylacine --sysroot=build/sysroot -march=...`, for plain-Makefile / autotools projects (musl, libsodium, stratumd); pins only what *defines* the target, leaving codegen policy to the consumer. `tools/build.sh sysroot` made real — creates the `build/sysroot/{include,lib}` skeleton + runs a toolchain self-check (`pouch-clang` must produce an aarch64 object). **Deviation from POUCH-DESIGN.md §9** (corrected in the same commit): the CMake toolchain file is named `Toolchain-aarch64-pouch.cmake`, not `-thylacine` — `Toolchain-aarch64-thylacine.cmake` is already the kernel toolchain; `-pouch` is role-named, parallel to the existing `-userspace`. TOOLING.md cross-compile section corrected (the stale `aarch64-unknown-thylacine` triple → `aarch64-thylacine`). Not audit-bearing (build scaffolding; no kernel code, no invariant surface). The sysroot is an empty skeleton — populated by sub-chunks 2-5. | `build.sh sysroot` exit 0 + toolchain self-check PASS (pouch-clang → aarch64 ELF object); the CMake toolchain configures a trivial project cleanly (Clang 22.1.4 resolved). No kernel-test-suite change — build scaffolding. |

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
- [ ] The patch series against vendored musl is documented + reproducible. *(sub-chunk 2: musl 1.2.5 vendored at `third_party/musl/`; the series is scaffolded + documented at `usr/lib/pouch/patches/`; the patches themselves land across sub-chunks 3-12.)*

## Build + verify commands

Standard (per `CLAUDE.md`): `tools/build.sh kernel [--sanitize=ubsan]` then `tools/test.sh` (`BOOT_TIMEOUT=420` for UBSan). New for this phase: `tools/build.sh sysroot` (becomes real at sub-chunk 1). TSan matrix matters from chunk 8 (`pouch-threads`) onward.

## Trip hazards

- **Chunks 7–8 are the hardest.** The `torpor` wait-on-address primitive + the pthread mutex/condvar layer. Spec-first (`futex.tla`); TSan from the start. Either may split.
- **musl's lower half may resist a clean seam** (POUCH-DESIGN.md risk R2). **Sub-chunk 2 probed this and R2 is substantially de-risked**: musl 1.2.5 compiles end-to-end for `aarch64-thylacine` (1345 TUs, 0 errors / 0 warnings → a valid `libc.a`), so the boundary line is *compile-clean*. The residual risk is not the compile — it is lower-half *behavior* complexity (the thread model, chunks 7-8), where the work is genuinely hard regardless of the seam.
- **Invariant P-1**: no foreign syscall number ever enters the kernel. Every kernel addition this phase is a *native Thylacine* syscall designed on Thylacine's terms — never "the Linux futex ported."
- **Stratum-side coordination** (chunks 14–15): the Thylacine `peer_creds` arm + possibly the block-layer arm are Stratum-repo changes.

## Known deltas from POUCH-DESIGN.md

No deviations. Sub-chunk 2 *resolved* an open question POUCH-DESIGN.md §4 left for the implementing chunk — the vendor path is `third_party/musl/` (not `usr/lib/pouch/musl/`); recorded in POUCH-DESIGN.md §4 + `docs/reference/78-pouch.md`. Deltas surfaced during implementation are recorded here per the deviation-tracking discipline.

## References

- `docs/POUCH-DESIGN.md` — binding design (authoritative).
- `docs/ROADMAP.md §7A` + `§2.1` + `§3.6`.
- `docs/ARCHITECTURE.md §16`, `§23`, `§25` (audit triggers), `§28` (invariants — pouch P-1..P-4 cross-ref).
- `docs/VISION.md §12` — Relationship to Linux and POSIX.
- `memory/project_pouch_phase.md`.
- Specs: `futex.tla` (#7), `notes.tla` (#8), `poll.tla` (#6), `pty.tla` (#9).
