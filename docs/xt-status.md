# XT -- x86 translation arc status (`docs/X86-TRANSLATION-DESIGN.md`)

The pickup guide for the x86 translation arc. The design document is the plan
and the research; this tracks what has LANDED and what is next. The arc sits
outside ROADMAP's eight phases and must never put the v1.0 release candidate at
risk (ROADMAP section 11). Branch: `claude/magical-shannon-sq5t3k`.

## TL;DR

**Ratified 2026-10-08.** The operator voted F1-F9 as recommended and F10 as G2:
- a native translator (FEX), with a hosted second decode (`svc #GUEST`) into
  Vivarium's single Linux personality;
- in-thread exact faults, the handler editing a Ureg;
- an image entitlement for code emission;
- translations stored as Stratum text, per user plus system;
- per-Proc address-space shapes;
- a TSO primitive where the hardware has one;
- spec-first for `fault_note` and `hosted_decode`;
- guest code generation as a per-objtype policy (`strict` for Linux amd64,
  `permissive` for Linux 386 and Windows).

The kernel never learns x86.

## Landed chunks

| Commit | What | Witness |
|---|---|---|
| `c847a99a` | `docs/WINE-STUDY.md`: Wine on Thylacine, the gap matrix, routes, graphics and Voodoo, and four defects found on the way (its Appendix A) | docs only |
| `94b7038a` | `docs/X86-TRANSLATION-DESIGN.md` DRAFT: principles P1-P7, kernel primitives XT-K1..K9, the runtime, the store, I-48, forks F1-F9 | docs only |
| `124ac831` | merge of `main` at `04df02c9` (134 commits, B-2 among them) | clean merge |
| `6018e567` | build: a fresh Linux clone builds the gate image. Two vendored crate files the ignore rules dropped are restored, the pouch, SDL2 and vkQuake patch series apply under GNU patch at fuzz 0, and each patched tree is byte-identical to the old series applied leniently | suite 1955/1955 on QEMU TCG (LLVM 18) |
| `dd0846c5` | design revised against B-2: citations re-anchored; debug authority (5.2); guest code provenance (5.4 item 4, I-48(e)); a fork carries text, not scratch (don't-fork code regions); F10 added | docs only |
| `f350cc76` (XT-0) | RATIFIED: `dec-2026-10-08-xt-design`, `dec-2026-10-08-xt-guest-code`, `arc-xt`; ARCH section 28 I-48 RESERVED and the XT amendments paragraph; CLAUDE.md row; ERRORS.md exact-fault contract; the NOVEL.md candidate; the tenth spec-first re-enablement | `tools/check-invariants.py` 48 rows; `quaestor lint` |
| `ab768299`, `5f0e65f3`, `cfb9201e` (XT-3a) | `SCTLR_EL1` composed whole on every entry path: the direct EL1 entry, the EL2 drop, PSCI secondaries (`start.S` `sctlr_el1_init_base`, `0x30D40998` after the audit). An EL0 `WFI` that would wait traps, and `exception.c`'s `EC_WFX` arm retires it (ELR + 4; SS and BTYPE cleared); `WFE` runs; SP alignment is checked at EL0 and EL1 on every path; BT1 is set with BT0. Each CPU records its final value (`hw_cpu_ident.sctlr_el1`). Fixes task #6. Audit r1 (cross-family): 0 P0 / 1 P1 / 1 P2 / 8 P3, all fixed but F10's event stream (task #15) | suite 1956/1956 on QEMU TCG (`-cpu max`); `hardening.sctlr_composed` (every online CPU, and the live register); `/hint-probe`; red-first: sabotage A fails the test, sabotage B kills the probe; `debug_step.tla` clean + 2 buggy cfgs red; SMP subset (1/4/8 CPUs, N=3): 9/9 PASS, 0 corruption. HVF and KVM runs owed (no such host here) |
| `dec9cd55`, `3638a075`, `3da0ac5c`, `541bb193`, *(pending)* (XT-3b) | Per-thread reaping, model-first (`specs/thread_reap.tla`). A thread that exits while a peer lives on RETIRES in its EXITING hold (off `p->threads` onto `p->exited`), so `thread_count` and `PROC_THREAD_MAX` count live threads. A live peer claims it once its switch away has settled, at that peer's next spawn or exit, then commits it (its run time and stack depth folded, and the unlink, in one hold) and frees it; exec drains the retired list before the swap; `wait_pid` takes the rest with the zombie. `preempt_check_irq` refuses an EXITING thread, and the last Thread out's /srv, /cap and weft teardown now runs before its EXITING commit, so on both exit paths the tail is the clear-child-tid handoff and `sched()`. Fixes task #7 (study F3), task #19 (exec freed the old address space under a retired tail), task #18's finding (both spawn handlers read `nt->tid` after `ready()`) and task #20 (an EXITING tail was preemptible at the `userland_enter` die-check). Audit (cross-family, Fable 5.1 at max effort): r1 0 P0 / 0 P1 / 0 P2 / 7 P3, r2 (on the fixes) 0 / 0 / 0 / 3, all fixed | suite 1961/1961 on QEMU TCG, default and UBSan; SMP subset 9/9 (1, 4 and 8 CPUs, 3 boots each, 0 CORRUPTION) and UBSan 4 CPUs 3/3; `/thread-torture` at boot and as a user (`ls-ci.exp` leg (f), first attempt); `check-thread-reap.sh`: 2 clean cfgs (858 / 5994 states, reap rounds modelled) and 6 buggy cfgs as claimed; red-first: sabotage A' and B failed exactly their predicted 5 and 4 tests. HVF and KVM runs are owed by the operator's hosts |

## Next

Sequence (the design's arc order, with two owned defects pulled forward):

1. **XT-3a** closed (audit r1, SMP 9/9). HVF and KVM gate runs are owed by the operator's hosts.
2. **XT-3b** closed (two audit rounds, SMP 9/9, UBSan, ls-ci). HVF and KVM gate
   runs are owed by the operator's hosts.
3. **PAC keys per address space** (task #5, study F1): today one key set is
   shared by the kernel's `pac-ret` and every EL0 process.
4. **XT-1: exact faults**, `specs/fault_note.tla` first.

Then XT-2, the rest of XT-3 (MRS emulation, `AT_HWCAP2`), XT-4 to XT-7 (kernel),
and XT-8 onward (the runtime).

## Open queue (owned; each is a tracked task)

- #5 PAC keys shared between the kernel and every EL0 process (study F1).
- #6 EL2-entry `SCTLR_EL1` leaves `nTWE`/`nTWI` clear (study F2) -> XT-3a.
- #7, #18, #19 and #20 closed by XT-3b: the lifetime cap; no `struct Thread *`
  used after its Thread can be reaped; exec's drain before the swap; the
  EXITING preempt gate.
- #8 documentation drift the study surfaced (its Appendix A, F4).
- #12 `build_tyrquake` extracts an LHA archive with `/usr/bin/tar`, which is bsdtar
  only on macOS (unverified here: the shareware data is unreachable).
- #13 Stratum: host `stratum-mkfs` does not link under GCC + GNU ld (link order;
  clang + lld links). Stratum fixes are pushed by the operator.
- #14 watch: the boot stack's high-water mark is 15536/16384 B in the test
  build (LLVM 18), a margin of 848 B.
- #15 XT-3c: enable the EL0 timer event stream (`CNTKCTL_EL1.EVNTEN`) before
  FEX's `WFE` spin-waits need it (audit XT-3a F10).
- #17 design: `/ctl` leaves re-render on every read, so a read-to-EOF reader
  can see a torn tail (`cat`, `read_to_end`, Linux guests); `/ctl/procs` also
  truncates at 4 KiB without a sign. `ps` and `cpubench` now read once
  (`41df9a8a`, task #16's fix); the systemic choice, a snapshot per open or the
  single-read contract, is open.
- #22 hygiene: `dyz_start_clunk` and its statics in `test_9p_client.c` are dead
  since tagpool TP-1 rewrote the two tests that used them.
- #23 investigate: `boot-wc`'s TOTAL `max_ms` reads 1-5.5 s in KERNEL_TESTS
  boots on TCG (pre-existing), while TICKLESS-IDLE.md says a starved periodic
  park ends within a tick; measure which park it is.
- #24 harness: the ls-ci timings table records the REQUESTED accel (`hvf`) when
  `run-vm.sh` falls back to TCG, because the spawn path logs no resolved accel.

## Building in a Linux container (what this branch's gates ran on)

Environment-only; none of this is in the tree:
- LLVM 18 linked at `/opt/homebrew/opt/{llvm,lld}`, the prefixes the toolchain
  files name.
- Host libsodium 1.0.20 built out of tree from `third_party/libsodium`
  (`PKG_CONFIG_PATH`). Ubuntu ships 1.0.18; Stratum wants >= 1.0.19.
- `CC=clang CXX=clang++ LDFLAGS=-fuse-ld=lld` for the host Stratum tools (#13).
- `THYLACINE_BAKE_DOSBOX=0`, because there is no LLVM fork here.
- A placeholder `build/quake/stage/id1/pak0.pak`, because the shareware mirrors
  are blocked by the container's network policy. So the Quake gates cannot run.
- No KVM: QEMU TCG, under which a boot takes about 160 s.
