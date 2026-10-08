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
| (XT-0) | RATIFIED: `dec-2026-10-08-xt-design`, `dec-2026-10-08-xt-guest-code`, `arc-xt`; ARCH section 28 I-48 RESERVED and the XT amendments paragraph; CLAUDE.md row; ERRORS.md exact-fault contract; the NOVEL.md candidate; the tenth spec-first re-enablement | `tools/check-invariants.py` 48 rows; `quaestor lint` |

## Next

Sequence (the design's arc order, with two owned defects pulled forward):

1. **XT-3a: `SCTLR_EL1` composed explicitly** (fixes task #6, study F2). EL2-entry
   boots write `0x30D00800`, and the MMU enable only ORs in M, C and I, so the EL0
   trap bits differ between boot paths. On the Pi, an EL0 `WFI`/`WFE` kills the
   Proc. Compose one value for both paths, with `CTR_EL0` readable at EL0
   (XT-K8); keep `UCI` off (B-2a).
2. **XT-3b: thread reaping** (fixes task #7, study F3). An exited thread is
   freed only when the whole Proc is reaped (`kernel/proc.c:6075-6082`), so
   `PROC_THREAD_MAX` counts a Proc's lifetime spawns. Gate: more than 1,000
   spawns in one Proc.
3. **PAC keys per address space** (task #5, study F1): today one key set is
   shared by the kernel's `pac-ret` and every EL0 process.
4. **XT-1: exact faults**, `specs/fault_note.tla` first.

Then XT-2, the rest of XT-3 (MRS emulation, `AT_HWCAP2`), XT-4 to XT-7 (kernel),
and XT-8 onward (the runtime).

## Open queue (owned; each is a tracked task)

- #5 PAC keys shared between the kernel and every EL0 process (study F1).
- #6 EL2-entry `SCTLR_EL1` leaves `nTWE`/`nTWI` clear (study F2) -> XT-3a.
- #7 `PROC_THREAD_MAX` is a lifetime cap (study F3) -> XT-3b.
- #8 documentation drift the study surfaced (its Appendix A, F4).
- #12 `build_tyrquake` extracts an LHA archive with `/usr/bin/tar`, which is bsdtar
  only on macOS (unverified here: the shareware data is unreachable).
- #13 Stratum: host `stratum-mkfs` does not link under GCC + GNU ld (link order;
  clang + lld links). Stratum fixes are pushed by the operator.
- #14 watch: the boot stack's high-water mark is 15536/16384 B in the test
  build (LLVM 18), a margin of 848 B.

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
